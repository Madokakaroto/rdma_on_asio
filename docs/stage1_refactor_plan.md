# Stage 1 Refactor Plan — Decoupled device discovery + centralized config

> Working/intermediate doc (like the earlier plan files) — delete after the refactor lands.
> Status: **awaiting "开始重构"**. No code changed yet.

## Goal

Separate **device discovery** (io_context-independent) from **completion-service
initialization** (per io_context), and make the operating config a single source of truth
held by the per-io_context service. This is the prerequisite for sharing one device across
multiple io_contexts, and for the io_context-free poll mode (Stage 2).

## Locked decisions

1. **One config type only** — `rdma_config_t`. No `rdma_device_config_t` (dropped). The same
   struct plays three conceptual roles (below); we do not introduce separate types.
2. **Discovery via the manager** — `rdma_device_manager_t::instance().get_first_available_device(ps, config)`.
3. **`use_device` returns `void`** and only takes an explicit `device_ptr`. Auto-discover and
   (nd) all `device_selector` overloads are removed.
4. **`connector`/`listener` `open()` drop the config param.** `listener::listen(backlog)` keeps
   its argument (asio-aligned); `rdma_config_t::backlog_` member is retained but unused for now.
5. **Connector/listener require `use_device` on their io_context** — otherwise throw (or set
   `ec`).
6. **Connection params come from the service's `effective_config_`** (not per-object config).
7. **Device-manager method names unified** — `for_each_device` + `get_first_available_device`
   on both backends.
8. **QP config is deferred to Stage 2.**

### The three config roles (one struct, `rdma_config_t`)

| role | what | where it lives | source |
|---|---|---|---|
| selection constraint | "find a device that can do *at least* this" | arg to `get_first_available_device(ps, config)` | user |
| device capability | what the HW supports (`max_cqe`, `max_qp_wr`, `max_qp_rd_atom`, …) | `device->attr_` / nd `info_` | hardware |
| effective/operating config | concrete values used to build CQ/QP/conn | `io_completion_service::effective_config_` | `derive_effective_config(user_config, caps)` at `use_device` |

`is_config_compatible`: a device matches iff every **non-zero** constraint field is `≤` the
device cap; a `0` field is a wildcard.

---

## File-by-file changes

### A. rdma layer — `include/rdma/rdma_types.hpp`
Add the manager alias under each backend block:
```cpp
// ASIO_RDMA_BACKEND_VERBS
using rdma_device_manager_t = ibv_device_manager_t;
// ASIO_RDMA_BACKEND_ND
using rdma_device_manager_t = nd_device_manager_t;
```
No `rdma_device_config_t`. `get_first_available_device(ps, config)` already takes
`ibv_config_t`/`nd_config_t` (= `rdma_config_t`).

### B. `use_device` — final two-overload surface (both backends)
`include/ibv/ibv_use_device.hpp`, `include/nd/nd_use_device.hpp`:
```cpp
void use_device(io_context&, device_ptr const&, config = {}, error_code&);
void use_device(io_context&, device_ptr const&, config = {});   // throws
```
Body:
```cpp
auto& svc = asio::use_service<detail::ibv_io_completion_service>(io_ctx);
if (svc.is_initialized()) { ec = ext_already_registered; return; }
if (!device)             { ec = ext_invalid_device;     return; }
svc.initialize(device, config, ec);
```
Remove from nd: auto-discover `use_device(io, config={})`, the `using device_selector = …`
alias and both `use_device(io, selector[, ec])` overloads, plus now-unused
`<functional>`/`<optional>` includes. (`io_completion_service::initialize(device, config, ec)`
and `effective_config_` / `get_effective_config()` already exist on both backends — no change.)

### C. Device manager — unify naming · `include/nd/nd_device.hpp`
- Rename nd `for_each_adapter` → `for_each_device` (callback shape `func(device_ptr const&) -> bool`
  already identical). Update its callers in `nd_use_device.hpp` (which are being deleted anyway).
- `get_first_available_device(ps, config)` already signature-identical. (ibv ignores `ps`; nd
  uses it for v4/v6 — invisible to callers.)

### D. Connector — drop config; source conn-params from `effective_config_`
**Public** (`include/ibv/ibv_connector.hpp`, `include/nd/nd_connector.hpp`):
- `open(ps)` — remove `config` param (ec + throwing forms).
- opening ctor `connector(io, ps, config)` → `connector(io, ps)`.
- `assign(handle)` / internal `assign_with_private_data(...)` — remove `config` param.

**Service** (`include/ibv/detail/ibv_connector_service.hpp`, `include/nd/detail/nd_connector_service.hpp`):
- Drop `config` from `open`/`do_open`/`assign`; remove the per-impl `config_` member.
- **`is_initialized()` guard:** in `open()` (fail-fast) and at `async_connect`/`async_accept`
  initiation (covers the auto-open path) — `use_service<io_completion_service>(this->context())`;
  if `!is_initialized()` → throw / set `ec`.
- **conn-param wiring** — read `auto eff = io_svc.get_effective_config();` then:
  - `responder_resources = eff.inbound_read_limit_`
  - `initiator_depth     = eff.outbound_read_limit_`
  - `rnr_retry_count = 7` (unchanged; no config field).
  - **ibv server accept**: replace hardcoded `1/1` at `ibv_connector_service.hpp:268-269`.
  - **ibv client connect**: values are built in the CM state machine at
    `ibv_op_connect.hpp:124-128` (the `TODO`). `async_connect` reads `eff` and passes
    `responder_resources`/`initiator_depth` into the `ibv_connect_op` ctor; the op stops
    hardcoding.
  - **nd connect + accept**: change `nd_connector_service.hpp:299-300,317-318` from
    `impl.config_.{in,out}bound_read_limit_` → `eff.{in,out}bound_read_limit_`.

### E. Listener — drop config; keep `listen(backlog)`
- `include/ibv/ibv_listener.hpp`, `include/nd/nd_listener.hpp`: `open(ps)` drops `config`;
  `listen(backlog = 128)` unchanged.
- Services: drop `config` from `open`; drop per-impl `config_`; add the `is_initialized()`
  guard in `open()`. `listen()` keeps using the `backlog` argument verbatim
  (`ibv_listener_service.hpp:115`).

### F. Tests — migrate every `use_device` call site
New pattern (return is `void`; device comes from the manager, so the `svc` local disappears):
```cpp
auto dev = rdma_device_manager_t::instance().get_first_available_device(tcp::v4(), {});
use_device(io_ctx, dev);
// use `dev` directly for MR / completion_queue / etc.
```
Sites to migrate:
- `tests/rdma/test_rdma_echo.cpp:166`, `tests/rdma/test_rdma_echo_poll.cpp:221`
- `tests/ibv/test_ibv_echo.cpp:164`, `tests/ibv/test_ibv_echo_poll.cpp:217`,
  `tests/ibv/test_ibv_connector_listener.cpp:62`
- `tests/nd/test_nd_echo.cpp:164`, `tests/nd/test_nd_echo_poll.cpp:218`,
  `tests/nd/test_nd_refactored_compile.cpp:22,47`
- `tests/nd/test_nd_use_device.cpp` — **rewritten**: it currently tests discover-inside-`use_device`
  (default-config + selector), which is removed; becomes a test of `get_first_available_device`
  + `use_device(io, dev)`.
- (Optional) negative test: connector/listener opened without `use_device` → expect throw / `ec`.

### G. Docs — `CLAUDE.md` + `README.md`
Document the new flow (`device_manager → get_first_available_device → use_device(io, dev, config)`),
the three config roles, "config centralized in the service; connector/listener read
`effective_config_`", and the new ordering rule (`use_device` before connector/listener).

---

## nd backend notes (parity details)

- **Selection function**: the manager's `get_first_available_device(ps, config)` uses
  `detail::is_valid_adapter(adapter, config)` ([nd_device_impl.hpp:576](include/nd/detail/nd_device_impl.hpp#L576)),
  *not* `is_config_compatible`. It encodes the same semantics as ibv's `is_config_compatible`
  (each field: `config.X > cap` → reject; `0` passes as wildcard). The Stage-1 "selection
  constraint" description applies to both; only the function name/location differs.
- **Dead code after Stage 1**: `detail::is_config_compatible` in `nd_config_derive.hpp` is used
  only by the auto-discover `use_device` being removed → it becomes unused. Remove it (and any
  now-unused `<functional>`/`<optional>` includes from `nd_use_device.hpp`).
- **Family / port-space coupling**: nd adapters *are* bound to a v4/v6 family (the manager
  filters via `ps.get_adapters(provider)`), unlike ibv (family resolved at connect time). So the
  device chosen via `get_first_available_device(tcp::v4(), …)` must match the family the
  connector/listener is opened with. Document this in the cross-platform usage notes; ibv ignores
  it harmlessly.
- **conn-param re-source (point D, nd flavor)**: nd already feeds read-limits into its
  `connect()`/`accept()` wrappers as explicit args from `impl.config_`
  ([nd_connector_service.hpp:299-300,317-318](include/nd/detail/nd_connector_service.hpp#L299)).
  The change is purely the *source*: read `use_service<nd_io_completion_service>(this->context())
  .get_effective_config()` instead of the per-connector `impl.config_`. (Contrast ibv, which
  builds an `rdma_conn_param` and currently hardcodes `1/1`.)
- **Guard reachability**: nd connector/listener services can reach the io-completion-service via
  `this->context()` (same pattern the verbs_service already uses), so the `is_initialized()` guard
  is feasible identically to ibv.

## Order of work + verification
1. **A + C** (aliases + nd manager rename) — additive/rename; compile both backends.
2. **B** (`use_device(io, dev, config)` returning void; remove old overloads) + **F** migration —
   build; run ibv/rdma echo (event mode) over RoCE → no regression.
3. **D + E** (drop config / guard / conn-param re-source) — rebuild all; rerun echo end-to-end;
   verify a non-default `inbound/outbound_read_limit_` actually reaches the negotiated connection
   (this is a real behavioral fix on ibv, which previously hardcoded `1/1`).
4. **G** docs.

## Open micro-decision
- Error code for "connector/listener opened without `use_device`": reuse `ext_invalid_device`,
  or add a dedicated `ext_device_not_registered` (recommended — clearer message).

## Explicitly out of scope (Stage 2)
- Unifying `rdma_queue_pair` (event `(io)` vs poll `(cq)` ctors), io_object_impl vs io_context-free
  poll mode, QP config source, and rewriting the poll-mode echo tests to complete on the poll thread.
