# Stage 2 Refactor Plan — io_context-free poll-mode queue_pair (no io_object_impl)

> Working/intermediate doc — delete after the refactor lands.
> Status: **under review**. No code changed yet. Depends on Stage 1 (centralized config,
> `use_device(io, device_ptr)`, `completion_queue` holding `device_`).

## Goal

Make `rdma_queue_pair` support two **mutually exclusive** construction modes with one type:

- `rdma_queue_pair(io_context&)` — **event mode**: completions bridged into the io_context
  (epoll/IOCP) via the shared CQ from `use_device`.
- `rdma_queue_pair(rdma_completion_queue&)` — **poll mode**: io_context-free data plane;
  completions reaped by the user's `cq.poll()`.

`(io_context, completion_queue)` is removed (ambiguous — already agreed). The `config` param is
dropped from both ctors (config read from the holder — pending **Q5**).

## Review of the proposed approach (the 4 points)

All four are correct:

1. **io_object_impl can't back both ctors.** ✔ `io_object_impl(0,0,io_ctx)` requires an
   execution_context. The poll-mode QP has none → io_object_impl is out. Confirmed against the
   current QP/service code.
2. **The branch only decides where {cq, device, config} come from.** ✔ event → from
   `io_completion_service`; poll → from the `completion_queue` (which already holds `device_` and
   its derived config). (Note: it's not just the CQ — the **device/PD** source differs too.)
3. **Dropping io_object_impl ⇒ hand-write the rule of five.** ✔ — with a per-backend twist
   (see §Ownership).
4. **Interaction with the verbs_service via static methods + a stored io_context\*.** ✔ This is
   the right shape: event mode reaches the service through the io_context; poll mode calls
   static helpers that take {device, pd, cq, qp, config} explicitly, so they don't touch the
   io_context.

### Two gaps the 4 points don't cover (must be designed)

**Gap A — completion executor.** Every data-plane op is `rdma_*_op<Buf, Handler, IoExecutor>`
and completes through `handler_work<Handler, IoExecutor>`. The static post helpers solve the
*posting* side (just need `qp_`), but the *completion* side still needs an executor. Resolution:
the QP holds one type-erased `asio::any_io_executor ex_`:
- event mode: `io_ctx.get_executor()` → `handler_work` posts to the io_context (today's behavior).
- poll mode: `asio::system_executor{}` → `handler_work` dispatches **inline on the poll thread**,
  i.e. the handler fires inside `cq.poll()`.

Using `any_io_executor` means the op type instantiates **once** for both modes (no template
explosion). **[DECIDED — Q3]** type-erased `any_io_executor` is accepted (one instantiation,
negligible per-completion indirection).

> **Important nuance (the actual io_context-free property):** `handler_work` uses the *handler's
> associated executor* when it has one, and falls back to `ex_` otherwise. So:
> - a **plain callback / future** token (no associated executor) → completes via `ex_` →
>   **inline in `poll()`** → truly io_context-free. ✅
> - a `co_await` / `use_awaitable` token carries the coroutine's executor → `handler_work` uses
>   *that*, so completion posts to the coroutine's io_context regardless of `ex_`.
>
> Conclusion: the QP design *enables* io_context-free poll mode, but it's realized only with
> non-io_context-bound tokens. The rewritten poll test must use callbacks/futures to actually
> demonstrate it (see §Tests).

**Gap B — immediate-completion sink** (empty-buffer fast path + synchronous post error).
**[DECIDED — Q1]** Today `finish_post`/`post_immediate_completion` routes these to
`scheduler_.post_immediate_completion` (the io_context). Poll mode has no scheduler. The
`completion_queue` gains a small intrusive **ready-op queue**; `poll()`/`poll_one()` drain it
alongside real CQEs. The poll-mode immediate-completion sink pushes the op there, preserving the
invariant "handlers only fire during `poll()`". (Inline-in-initiating-call alternative rejected —
breaks that invariant.)

## queue_pair data structure — reuse `implementation_type` + `io_context*`

The queue_pair holds the service's `implementation_type` **directly** (no `io_object_impl`) plus
one discriminator pointer:

```cpp
class ibv_queue_pair {
  detail::ibv_verbs_service::implementation_type impl_;
  asio::io_context* io_ctx_ = nullptr;   // null ⇒ poll mode  (the mode discriminator)
};
```
`io_ctx_ != nullptr` ⇔ event mode. (`impl_.service_cq_mode_` becomes redundant with `io_ctx_ !=
nullptr` — drop it, or keep as a derived mirror.)

### `implementation_type` gets three new fields

Today it has `qp_, cq_, comp_channel_, service_cq_mode_, config_`. The poll-path statics need
data that today comes from the io_context, so add:

| add | why |
|---|---|
| `device_ptr device_` | `create_qp` no longer looks the device up via `io_completion_service` — it reads `impl_.device_` (event: from the service; poll: from the completion_queue). Also keeps the device/PD alive for the QP's lifetime. |
| `any_io_executor ex_` | the op's `IoExecutor` (event: `io_ctx.get_executor()`; poll: `system_executor`). Statics/members read it from `impl_`. |
| `rdma_completion_queue* poll_cq_` | poll-mode immediate-completion **sink** — the static error/empty-buffer path pushes to `poll_cq_`'s ready-op queue (Gap B). Null in event mode. |

So the full `implementation_type` becomes: `{ qp_, cq_, comp_channel_, config_(effective),
device_, ex_, poll_cq_ }` (and `service_cq_mode_` dropped). The same struct is reused by both
backends (nd's already owns `qp_` via `nd2_queue_pair_ptr`; ibv's `qp_` stays a raw non-owning
pointer).

### Dispatch: event = service **member**, poll = service **static**

`queue_pair::async_send(bufs, token)` branches on `io_ctx_`:
```
if (io_ctx_)  use_service<ibv_verbs_service>(*io_ctx_).async_send(impl_, bufs, handler, impl_.ex_);
else          ibv_verbs_service::async_send_static(impl_, bufs, handler, impl_.ex_);   // sink = impl_.poll_cq_
```
Event members reach `scheduler` (immediate-completion) + `io_completion_service` (`arm_notify`)
through the service object; statics use `impl_.poll_cq_` as the sink and skip `arm_notify`. Both
operate on the *same* `impl_` and call the same post/sglist core.

### The service stops *owning* `implementation_type`

Because the queue_pair holds `impl_` directly, `ibv_verbs_service` no longer needs its
io_object_impl plumbing: `construct/destroy/move_construct/move_assign`, the `impl_list_`
tracking, and `implementation_type`'s `base_implementation_type` inheritance all become unused →
remove them. The service is reduced to: reactor/scheduler refs (event-mode `ibv_service_base`) +
member methods (event) + static methods (poll), all taking `impl_&`.

## verbs_service refactor — static core + thin event wrappers

Keep `ibv_verbs_service` / `nd_verbs_service` as the home of the verbs logic, but split each
operation into a **static primitive** (no io_context) and an **event-mode member wrapper**:

```
// static core — used directly by poll mode; called by the event wrappers too
static error_code create_qp(device_ptr, native_cq_t* cq, config, native_cm_id_t* cm_id /*ibv*/);
static void post_send_static(native_qp_t*, op*, ...);   // post + on error, push to a given sink
//   ... post_recv/read/write_static, parameterized by an immediate-completion sink callback ...

// event-mode member wrappers (need the io_context): fetch device/cq from io_completion_service,
// arm_notify after a successful post, route immediate completions to the scheduler, then
// delegate to the static core.
```

The event wrappers become *fetch-and-forward*: pull `{device, cq}` + executor + sinks from the
io_completion_service / scheduler, then call the same statics poll mode uses. This avoids
duplicating the post/sglist logic.

**[DECIDED — Q2]** `ibv_verbs_service` / `nd_verbs_service` **remain per-context services**
(registered via `use_service`, used by event-mode QPs). They **additionally** expose the static
interface consumed by poll-mode QPs. So each service has two faces:
- *member* methods (event mode) — require the io_context; fetch `{device, cq, scheduler,
  io_completion_service}` and forward to the statics;
- *static* methods (poll mode) — take `{device, pd, cq, qp, config, sink}` explicitly, touch no
  io_context. The members are implemented in terms of the statics.

## Construction paths & `create_qp` (per backend) — [Q4 detail, for review]

`create_qp` becomes a **static primitive** on the verbs service that takes everything explicitly
and touches no io_context:
- **nd**: `static nd2_queue_pair_ptr create_qp(nd_adapter_ptr const& adapter, native_cq_t* cq,
  nd_config_t const& effective, error_code&)` — builds `native_qp_init_attr{ .rcq_=cq, .icq_=cq,
  .max_*_=effective.* }`, then `verbs_ops::create_qp(adapter->pd_.get(), attr, ec)`. (Lifts the
  body currently duplicated in the two `open` overloads at
  [nd_verbs_service.hpp:110-120 / 156-166](include/nd/detail/nd_verbs_service.hpp#L110).)
- **ibv**: `static error_code create_qp(device_ptr const& dev, native_cq_t* cq,
  ibv_config_t const& effective, native_cm_id_t* cm_id)` — fills `ibv_qp_init_attr{ .send_cq=cq,
  .recv_cq=cq, ... }` and `rdma_create_qp(cm_id, dev->pd_.get(), &attr)`. (Drops the
  `use_service<io_completion_service>(this->context())` device lookup at
  [ibv_verbs_service.hpp:105-108](include/ibv/detail/ibv_verbs_service.hpp#L105) — device now
  comes in as a param.)

### nd — QP created at **construction** (nd has no deferred create)

`nd_queue_pair(rdma_completion_queue& cq)` (poll mode, io_context-free):
1. `adapter = cq.device()`, `native_cq = cq.native_handle()`, `effective = cq.effective_config()`.
2. `qp_ = nd_verbs_service::create_qp(adapter, native_cq, effective, ec)` → **the queue_pair owns
   this `nd2_queue_pair_ptr`**. (throws / sets `ec`.)
3. Store `poll_cq_ = &cq`, `device_ = adapter`, `config_ = effective`,
   `ex_ = asio::system_executor{}`, `io_ctx_ = nullptr`.

`nd_queue_pair(io_context& io)` (event mode):
1. `io_svc = use_service<nd_io_completion_service>(io)`; `!is_initialized()` → throw / `ec`.
2. `adapter = io_svc.get_device()`, `native_cq = io_svc.get_cq()`,
   `effective = io_svc.get_effective_config()`.
3. `qp_ = nd_verbs_service::create_qp(adapter, native_cq, effective, ec)` (same static).
4. Store `poll_cq_ = nullptr`, `device_ = adapter`, `config_ = effective`,
   `ex_ = io.get_executor()`, `io_ctx_ = &io`; cache `io_svc` + `scheduler` (for `arm_notify` and
   the event-mode immediate-completion sink).

Both nd ctors converge on the same static `create_qp`; only the *source* of `{adapter, cq,
effective}` and the *executor/sink* differ. The connector receives the already-created QP via
`qp.native_handle()` (nd connector takes `native_qp_t*` — no `make_create_qp_fn`).

### ibv — QP creation **deferred to the connector** (unchanged timing)

ibv does *not* create the QP at construction. Both ctors only record state:

`ibv_queue_pair(rdma_completion_queue& cq)` (poll): `poll_cq_=&cq`, `device_=cq.device()`,
`config_=cq.effective_config()`, `ex_=system_executor{}`, `io_ctx_=nullptr`, `qp_=nullptr`.

`ibv_queue_pair(io_context& io)` (event): fetch+check `io_svc`; `device_=io_svc.get_device()`,
`config_=io_svc.get_effective_config()`, `ex_=io.get_executor()`, `io_ctx_=&io`, `qp_=nullptr`;
cache `io_svc`+`scheduler`.

The actual QP is built later: the connector calls `qp.make_create_qp_fn()` during
connect/accept; the returned lambda calls the static `create_qp(device_, native_cq_of(poll_cq_ or
io_svc), config_, cm_id)` and assigns `qp_ = cm_id->qp` (**non-owning** — connector owns it). No
io_context is touched on the poll path.

> **QP config source [needs your nod — Q5].** Consistent with Stage 1 (config centralized, not
> repeated per object), I propose **dropping the `config` param from both QP ctors**: event mode
> reads `io_svc.get_effective_config()`, poll mode reads `cq.effective_config()`. The user already
> sets caps at the holder (`use_device(io, dev, config)` / `completion_queue(dev, config)`).
> Trade-off: no per-QP cap override (all QPs on a holder share its effective config). If you want
> per-QP overrides, we keep an optional `config` param instead. Recommended: drop it.

## Ownership & rule of five

Holding `impl_` directly makes the rule-of-five **uniform** at the queue_pair level — *move
`impl_`, copy+null `io_ctx_`* — because per-backend ownership is already encoded in the type of
`impl_.qp_`:

- **ibv**: `impl_.qp_` is `native_qp_t*` (**non-owning**; `cm_id->qp`, destroyed by the connector)
  → moving/clearing `impl_` doesn't free the QP. Good.
- **nd**: `impl_.qp_` is `nd2_queue_pair_ptr` (**owning**) → `impl_`'s move moves the smart ptr and
  `impl_`'s destruction releases the QP. Good.

So the queue_pair gets: defaulted move ctor/assign (member-wise move of `impl_` — the nd smart
ptr nulls its source automatically; for ibv null the raw `qp_`/`poll_cq_` in the move to avoid a
dangling moved-from), deleted copy, default ctor = empty (deferred `open(io)` / `open(cq)`
allowed), destructor = whatever `impl_`'s members do (nd frees the QP; ibv no-ops).

Move is safe w.r.t. in-flight ops — ops hold only `handler_ + work_`, no back-pointer to the QP
or `impl_` (completion is resolved via CQ `wr_id`/`RequestContext`). Caveat to document: don't
keep issuing on a moved-from object; already-posted completions are unaffected.

## completion_queue changes

- Already holds `device_` (ibv + nd) — expose `device()` for the poll-mode QP to read.
- **Store + expose the full `effective_config_`**: today the ctor derives it but only keeps
  `cqe_` ([ibv_completion_queue.hpp:31](include/ibv/ibv_completion_queue.hpp#L31)). Persist the
  whole `derive_effective_config(config, device->attr_)` result and add `effective_config()`, so
  the poll-mode QP reads its caps from the CQ (this is the poll-side config source — Q5).
- Add the intrusive **ready-op queue** (Gap B) + drain it in `poll()`/`poll_one()` before/after
  the native poll loop.
- (No comp_channel — unchanged.)

## connector / listener

No API change. The connector keeps calling `qp.make_create_qp_fn()`; the lambda now sources
device/cq from the QP's mode (event: io_completion_service; poll: completion_queue). Control plane
stays io_context-bound (rdma_cm / ND connector) — only the **data plane** becomes io_context-free.

## nd backend specifics (parity details)

The implementation_type-reuse + static/member split applies symmetrically, with these
nd-specific points:

- **QP lifetime / handoff**: nd creates the QP at **construction** and **owns** it
  (`nd2_queue_pair_ptr impl_.qp_`); the connector takes the already-built QP via
  `qp.native_handle()` ([nd_connector.hpp async_connect/async_accept](include/nd/nd_connector.hpp))
  — there is **no `make_create_qp_fn`** on nd. So the static `create_qp(adapter, cq, effective)`
  runs inside both nd ctors (poll: from the completion_queue; event: from io_completion_service).
- **arm_notify**: nd's event-mode notify lives in `work_started()`
  ([nd_verbs_service.hpp:324](include/nd/detail/nd_verbs_service.hpp#L324)), gated on
  `impl.iocp_mode_`. After the refactor that gate becomes member(event)=arm vs static(poll)=skip,
  matching ibv's `service_cq_mode_` split.
- **Gap B sink**: nd's `post_immediate_completion`
  ([nd_verbs_service.hpp:339](include/nd/detail/nd_verbs_service.hpp#L339)) posts to
  `this->scheduler_` via `nd_complete_op` — same shape as ibv. The poll-mode static path needs a
  push-to-`completion_queue`-ready-queue equivalent (an op wrapper that the CQ's `poll()` drains),
  in place of the scheduler post.
- **Pre-existing empty-buffer divergence to fix**: nd's `start_*_op` currently does
  `if (all_empty(buffers)) return;` ([nd_verbs_service.hpp:244](include/nd/detail/nd_verbs_service.hpp#L244)),
  which **drops the op without completing it** (handler never fires, op leaked). ibv instead
  `post_immediate_completion(op)`. While extracting the statics, align nd to ibv: empty-buffer
  ops complete (event → scheduler; poll → ready-queue) with `bytes_transferred = 0`.
- **completion_queue (nd)**: `nd_completion_queue` owns an overlapped file handle + ND2 CQ and
  `poll()` calls `verbs_ops::poll_cq`. Add `effective_config()` (persist the derived config, today
  only `cqe_` is used) and the ready-op queue drained inside `poll()`/`poll_one()` — same as ibv.
- **io-completion-service**: `nd_io_completion_service` already exposes
  `get_device()/get_cq()/get_effective_config()/is_initialized()`
  ([nd_io_completion_service.hpp:85-90](include/nd/detail/nd_io_completion_service.hpp#L85)) — the
  event-mode member wrappers source `{device, cq, effective}` from it, no new API needed.

## Tests

- Rewrite `tests/rdma/test_rdma_echo_poll.cpp` + `tests/ibv/test_ibv_echo_poll.cpp` (and the nd
  twins) to demonstrate the **io_context-free data plane**:
  - control plane: `connect`/`accept`/`disconnect` still on the io_context (run `io.run()` to
    establish/tear down the connection).
  - data plane: drive `async_send`/`async_recv` with **callbacks or `use_future`** (NOT
    `use_awaitable`), completing **inline on the poll thread** inside `cq.poll()`. No `io.run()`
    on the data path.
  - the dedicated poll thread spins `cq.poll()` (now also draining the ready-queue).
- Fix the CLAUDE.md / comment claims that currently overstate "handlers fire synchronously inside
  poll()" — make them accurate (true only for non-io_context-bound tokens).
- Keep an event-mode echo as the regression baseline.

## Order of work + verification

1. **completion_queue**: expose `device()`, add ready-op queue + drain (additive; rebuild).
2. **verbs_service**: extract static primitives; reimplement event-mode members as
   fetch-and-forward over the statics. Run event-mode echo over RoCE → no regression.
3. **queue_pair**: drop io_object_impl; add `(io_context)` + `(completion_queue)` ctors +
   deferred `open` twins; hand-write rule of five (per-backend ownership); wire `any_io_executor`
   + immediate-completion sink. Remove `(io, cq)`.
4. **connector make_create_qp_fn**: source device/cq per mode.
5. **tests**: rewrite poll echo (callback/future, io_context-free data plane); rerun both modes
   end-to-end over RoCE.
6. **docs**: correct the poll-mode description.

## Decisions & open questions

- **Q1 — [DECIDED]** Gap B uses a `completion_queue` ready-op queue drained by `poll()`.
- **Q2 — [DECIDED]** `verbs_service` stays a per-context service **and** exposes a static interface
  for poll-mode QPs (two faces; members implemented over the statics).
- **Q3 — [DECIDED]** `any_io_executor` (type-erased) for the op's `IoExecutor`.
- **Q4 — [DETAILED ABOVE, awaiting your review]** nd creates the QP at construction (poll ctor
  builds it from the `completion_queue`'s device/cq, no io_context); ibv stays deferred-to-connect.
  See *Construction paths & `create_qp`*.
- **Q5 — [NEW, needs your nod]** Drop the `config` param from both QP ctors and read the effective
  config from the holder (io_completion_service / completion_queue)? Recommended: yes, for Stage 1
  consistency; the alternative keeps an optional per-QP override.
```
