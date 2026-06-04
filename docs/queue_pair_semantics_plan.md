# queue_pair Interface-Semantics Iteration — `bind` / `is_bound` (drop `open` / `is_open`)

> Not an interface overhaul like the Stage 1 / Stage 2 refactors — this iteration only aligns the
> queue_pair's semantics (and vocabulary) with RDMA concepts. Working/intermediate doc; delete
> after it lands.
> Status: **ready to implement** (decisions locked below).

## Goal

A queue pair must be associated with a **completion mechanism** before use. There are exactly
two, and they are *not* the same kind of object:

- **event mode** → the **io_context's internal, managed CQ** (comp_channel+epoll / IOCP),
  drained automatically by the reactor/IOCP. The user never handles this CQ directly.
- **poll mode** → a **user-created `rdma_completion_queue`** (device-only), drained by the user
  via `poll()` / `poll_one()`.

So the queue pair is associated with **either an `io_context` or a `completion_queue`**. The verb
for that association is **`bind`** (a QP *binds* to a completion mechanism) — not `open`. `open`
was an asio borrowing that (a) implied "create the native resource" and (b) gave an inconsistent
`is_open()` across backends. This iteration renames the surface to `bind` / `is_bound` and makes
the predicate consistent.

**`completion_queue` is unchanged** — it remains the poll-mode, user-facing CQ created from a
device only (no io_context ctor). It is never used to express event mode.

## queue_pair surface

```cpp
// construct + bind
rdma_queue_pair(asio::io_context& io);          // event mode (io_context's internal CQ)
rdma_queue_pair(rdma_completion_queue& cq);      // poll mode  (user CQ)

// deferred bind (default-construct, then bind later)
void bind(asio::io_context& io [, asio::error_code&]);
void bind(rdma_completion_queue& cq [, asio::error_code&]);

bool            is_bound()  const noexcept;   // bound to any completion mechanism?
completion_mode bound_type() const noexcept;  // which one: none / event / poll

// REMOVED: open(...) and is_open()
```

### `completion_mode` (backend-agnostic, in `rdma_commons.hpp`)

The two modes map 1:1 onto RDMA's two completion-notification mechanisms — **polling**
(`ibv_poll_cq` loop on a plain CQ) and **completion events** (`ibv_req_notify_cq` +
`ibv_get_cq_event` on a completion channel / ND IOCP). Names follow that (and the existing
"Event-driven mode" / "Poll mode" docs):

```cpp
enum class completion_mode {
  none,    // not bound to any completion mechanism yet
  event,   // event-driven: io_context's managed CQ, completion-channel/IOCP notification
  poll,    // polled: a user-owned completion_queue, reaped via poll()/poll_one()
};
```

`bound_type()` derives this from the existing internal state — no new members needed:
```cpp
completion_mode bound_type() const noexcept {
  if (impl_.cq_ == nullptr) return completion_mode::none;
  return io_ctx_ ? completion_mode::event : completion_mode::poll;  // poll => impl_.poll_cq_ set
}
```

The two constructors are unchanged in spelling from Stage 2 (`(io_context&)` / `(completion_queue&)`)
— only the *deferred* setter is renamed `open`→`bind` and the predicate `is_open`→`is_bound`.

### `is_bound()` is consistent (this is the substantive fix)

`is_bound()` means **"bound to a completion mechanism"** = `impl_.cq_ != nullptr` (the bound CQ
handle — the io_context's shared CQ in event mode, or the user CQ in poll mode). After `bind(...)`
(or the constructor) it is **true on both backends**.

This replaces `is_open()`, which was inconsistent: it returned `impl_.qp_ != nullptr` ("native QP
exists"), which is true on nd right after `open()` (nd creates the QP at bind time) but false on
ibv (ibv defers QP creation to the connector's `async_connect`/`async_accept`). That ibv/nd timing
difference is **intrinsic** — rdma_cm couples QP creation to the cm_id, ND does not — so we stop
exposing "native QP exists" as a predicate; `native_handle()` still returns the raw QP (null on
ibv until connect, set on nd) for those who need it.

## File-by-file

### rdma layer
- `rdma_commons.hpp`: add `enum class completion_mode { none, event, poll };` (backend-agnostic,
  in `asio::rdma`).

### ibv
- `ibv_queue_pair.hpp`: rename `open(io_context&[,ec])` → `bind(io_context&[,ec])` and
  `open(completion_queue&)` → `bind(completion_queue&[,ec])`; rename `is_open()` → `is_bound()`
  (= `impl_.cq_ != nullptr`); add `bound_type()` → `completion_mode`. Constructors keep delegating
  to `bind`. No semantic change to the event/poll dispatch internals (`io_ctx_` / `impl_.poll_cq_`).
- `ibv_verbs_service.hpp`: replace `is_open(impl)` (= `qp_ != nullptr`) with `is_bound(impl)`
  (= `cq_ != nullptr`); keep `native_handle(impl)` = `impl.qp_`.

### nd (mirror; unverified on Linux)
- `nd_queue_pair.hpp`: same renames (`bind` / `is_bound`) + `bound_type()`; nd still creates+owns
  the QP inside `bind(...)`.
- `nd_verbs_service.hpp`: `is_bound(impl)` = `cq_ != nullptr`.

### completion_queue (ibv + nd)
- **No change.** Device-only ctor; `device()` / `effective_config()` / ready-queue already exist
  (Stage 2). No io_context ctor, no event flavor.

### Tests
- `test_nd_refactored_compile.cpp`: `qp.open(io_ctx)` → `qp.bind(io_ctx)`, `qp.is_open()` →
  `qp.is_bound()`.
- Echo / connector-listener tests use the **constructors** (`queue_pair qp(io)` /
  `queue_pair qp(cq)`), which are unchanged — so they need no edits. (Grep for any stray
  `.open(` / `.is_open(` on a queue_pair and convert.)

### Docs
- CLAUDE.md + README: queue_pair surface uses `bind` / `is_bound`; state the model "a QP binds to
  a completion mechanism — an io_context (event) or a completion_queue (poll)"; `completion_queue`
  is poll-mode only (device-only).

## Order of work + verification
1. ibv: rename in `ibv_queue_pair.hpp` + `ibv_verbs_service.hpp` (`bind`/`is_bound`). Build.
2. Convert any queue_pair `.open`/`.is_open` call sites. Run ibv + cross-platform echo (event +
   poll) over RoCE — no regression.
3. nd mirror.
4. Docs.

## Decisions (locked)
- **Verb:** queue_pair uses `bind` / `is_bound`; `open` / `is_open` removed.
- **completion_queue:** poll-mode only, created from a device; **no io_context ctor / no event
  flavor** (event mode uses the io_context's internal CQ, bound via `bind(io_context&)`).
- **`is_bound()`** = bound to a completion mechanism (`impl_.cq_ != nullptr`), consistent across
  backends; native-QP existence stays internal (behind `native_handle()`).
- **`bound_type()`** → `enum class completion_mode { none, event, poll }` (names from RDMA's
  polling vs completion-event mechanisms). Alternative spelling `event_driven` / `polled` was
  considered; `event` / `poll` chosen for concision + match with existing docs.
- Earlier Option-B ideas (exposing the event CQ as a first-class `completion_queue`,
  `completion_queue(io_context&)`, `io_context_or_null()`, `poll()`-on-event-CQ) are **dropped**.
