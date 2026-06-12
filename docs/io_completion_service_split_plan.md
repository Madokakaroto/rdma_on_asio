# Refactor Plan 鈥?split io_completion_service into device_service + io_completion_service

> Working/intermediate doc; delete after it lands. Not an interface change for users 鈥?this is an
> internal (detail/) single-responsibility split. Builds on the device-discovery + poll-mode work.

## Goal

Today `io_completion_service` carries **two** responsibilities:

1. **Device registration** 鈥?stores the user-supplied `device_ptr` + the derived
   `effective_config` (and the `is_initialized`/registered flag).
2. **Completion notification** 鈥?owns the shared CQ + comp_channel (epoll) / IOCP, registered on
   the reactor, and the `arm_notify()` poller that bridges CQ completions into the io_context.

Split into two single-responsibility per-`io_context` services:

- **`device_service`** 鈥?owns the registered `device_ptr` + `effective_config`; answers
  `is_registered()` / `get_device()` / `get_effective_config()`.
- **`io_completion_service`** 鈥?owns the shared CQ + comp_channel/IOCP notify + reactor poller +
  `arm_notify()`; answers `get_cq()` / `get_comp_channel()` / `is_initialized()`.

> Naming note: `device_service` is **per-io_context** (the device *this io_context uses* + its
> operating config), distinct from the process-wide `*_device_manager_t` (device *discovery*).

No inter-service dependency: services don't reference each other. `use_device` is the sole
coordinator that wires them; the io-objects read whichever service they need.

## The two services (both backends)

### `device_service` (new, detail/)
```
struct: device_ptr device_; config effective_config_; bool registered_ = false;

void register_device(device_ptr, config effective);   // store; sets registered_
bool is_registered() const;
device_ptr get_device() const;
config const& get_effective_config() const;
void shutdown();                                       // reset
```
Pure storage 鈥?no reactor/scheduler. (`execution_context_service_base`.)

### `io_completion_service` (slimmed)
- **Drops** `device_`, `effective_config_`, `get_device()`, `get_effective_config()`, the
  config-derivation in `initialize`, **and (Q2 = option C) the public `is_initialized()` +
  `initialized_` flag** 鈥?"is set up?" is now answered solely by `device_service::is_registered()`.
- **Keeps** `cq_`, `comp_channel_`, `cq_reactor_data_`, the `*_op_notify_wr` poller, `arm_notify()`,
  `get_cq()`, `get_comp_channel()`, reactor/scheduler refs.
- `initialize()` self-guards on `cq_ != nullptr` (idempotency) instead of a separate flag.
- `initialize` no longer takes a raw config; it takes the device (for `context_`/`adapter_`) + the
  already-derived CQ depth:
  ```cpp
  void initialize(device_ptr const& device, size_type cqe, error_code& ec);  // creates comp_channel + CQ
  ```
  The device is used transiently to create the CQ; it is **not stored**.

## use_device wiring (the coordinator)

`use_device(io, device, config)` registers the device and initializes the notify service, in an
order that stays atomic (don't register if CQ creation fails):
```cpp
auto& dev = use_service<device_service>(io);
if (dev.is_registered())          { ec = ext_already_registered;     return; }
if (!device)                      { ec = ext_invalid_device;          return; }
auto effective = derive_effective_config(config, device->attr_/info_);   // free fn, as today
auto& io = use_service<io_completion_service>(io_ctx);
io.initialize(device, effective.cqe_, ec);                               // CQ + comp_channel
if (ec) return;                                                          // not registered on failure
dev.register_device(device, effective);                                  // store device + effective
```
`derive_effective_config` is computed in `use_device` (it already owns device+config); `device_service`
just stores the result. (Alternative: have `device_service.register_device` derive internally and
roll back on CQ-init failure 鈥?rejected; the above is simpler and atomic.)

## Cached service references 鈥?no runtime `use_service` on hot paths

`asio::use_service<S>(ctx)` walks the service list comparing keys 鈥?non-trivial, and **must not
run per-operation**. asio's own services avoid this by caching a **reference** to each depended
service, obtained **once in the constructor initializer list**. `io_completion_service` already
does this (`reactor_(use_service<reactor>(ctx)), scheduler_(use_service<scheduler>(ctx))`); this
refactor extends the same discipline everywhere a service depends on another service.

Convert these (member ref + ctor-init-list `use_service`, never per-call):

| holder (a service) | depends on | used by | today |
|---|---|---|---|
| `verbs_service` | `io_completion_service&` | `arm_notify()` 鈥?**per data-plane op** | `use_service` per op 鉂?|
| `connector_service<PS>` | `device_service&` | connect/accept (`is_registered`/`effective_config`; nd also `get_device`) | `use_service` per call |
| `listener_service<PS>` | `device_service&` | open / get-connection (guard; nd also `get_device`) | `use_service` per call |

```cpp
// e.g. ibv_verbs_service
explicit ibv_verbs_service(asio::execution_context& ctx)
    : execution_context_service_base(ctx)
    , ibv_service_base(ctx)
    , io_completion_svc_(asio::use_service<ibv_io_completion_service>(ctx)) {}   // once
...
ibv_io_completion_service& io_completion_svc_;
```
Lifetime is safe: the depended service is created during the holder's ctor (so it's created
*before* and destroyed *after* the holder), and stays alive through the holder's `shutdown()` 鈥?the same guarantee asio relies on for `reactor&`/`scheduler&`.

**Non-service caller 鈥?`queue_pair`** (not a service, so no ctor-init-list): the event-mode
`async_*` path currently does `use_service<verbs_service>(*io_ctx_)` **per op**. Cache the service
pointer **once at `bind(io)`** (`verbs_svc_ = &use_service<verbs_service>(io)`) and have the async
ops use the cached pointer; poll mode leaves it null and uses the static path. The `device_service`
/ `io_completion_service` reads in `bind()` itself are once-per-QP (fine as-is).

> No new cycles: `device_service` depends on nothing; `io_completion_service` 鈫?reactor/scheduler;
> `verbs_service` 鈫?`io_completion_service`; `connector`/`listener` 鈫?`device_service`.

## Call-site migration

| current (`io_completion_service`) | 鈫?| new |
|---|---|---|
| `get_device()` | 鈫?| `device_service::get_device()` |
| `get_effective_config()` | 鈫?| `device_service::get_effective_config()` |
| `is_initialized()` used as the **"use_device called?" guard** | 鈫?| `device_service::is_registered()` (鈫?`ext_device_not_registered`) |
| `get_cq()` / `get_comp_channel()` | 鈫?| `io_completion_service` (unchanged) |
| `arm_notify()` | 鈫?| `io_completion_service` (unchanged) |

Sites (both backends):
- **use_device** (`ibv/nd_use_device.hpp`): wire both services as above.
- **queue_pair `bind(io)`** (`ibv/nd_queue_pair.hpp`): `device`/`config` from `device_service`,
  `cq` from `io_completion_service`; the `is_registered()` guard from `device_service`.
- **verbs_service** (`ibv/nd_verbs_service.hpp`): `arm_notify()` from `io_completion_service`
  (unchanged target, just the slimmed service).
- **connector_service** (`ibv/nd_connector_service.hpp`): guard `is_registered()` +
  `get_effective_config()` from `device_service`. **nd also** uses `get_device()` (to create the
  IND2Connector) 鈫?`device_service::get_device()`.
- **listener_service** (`ibv/nd_listener_service.hpp`): guard `is_registered()` from
  `device_service`. **nd also** uses `get_device()` (to create the listener) 鈫?  `device_service::get_device()`.

`completion_queue` (poll) is untouched 鈥?it never used `io_completion_service`.

## File-by-file
- **new:** `include/rdma/ibv/detail/ibv_device_service.hpp`, `include/rdma/nd/detail/nd_device_service.hpp`.
- **edit:**
  - `ibv/nd_io_completion_service.hpp` 鈥?slim down (drop device/config/`is_initialized`;
    `initialize(device, cqe, ec)` self-guards on `cq_`).
  - `ibv/nd_use_device.hpp` 鈥?wire both services (derive config; `io.initialize` then
    `dev.register_device`).
  - `ibv/nd_queue_pair.hpp` 鈥?`bind` reads device/config from `device_service`, cq from
    `io_completion_service`; guard via `device_service::is_registered()`; **cache `verbs_service*`
    at `bind(io)`** for the per-op event path.
  - `ibv/nd_verbs_service.hpp` 鈥?hold `io_completion_service&` as a ctor-init-list member; use it
    in `arm_notify`/`finish_event` (no per-op `use_service`).
  - `ibv/nd_connector_service.hpp` + `ibv/nd_listener_service.hpp` 鈥?hold `device_service&` as a
    ctor-init-list member; guard / `get_effective_config` / (nd) `get_device` via it.
- **docs:** CLAUDE.md service list (split the one bullet into two; note device_service vs
  device_manager).

## Order of work + verification (ibv first, then nd mirror)
1. Add `ibv_device_service`. Slim `ibv_io_completion_service` (drop device/config/`is_initialized`;
   `initialize` takes device + cqe, self-guards on `cq_`). Build.
2. Rewire `ibv_use_device`, `ibv_queue_pair::bind`, `ibv_connector_service`,
   `ibv_listener_service` 鈥?including the **cached-reference** conversions (verbs鈫抜o_completion,
   connector/listener鈫抎evice_service, queue_pair caches verbs_service at bind). Build; run ibv +
   cross-platform echo (event + poll) over RoCE 鈥?no regression.
3. Mirror to nd (unverified on Linux), including nd connector/listener `get_device()` from
   `device_service` and the same cached refs.
4. Docs.

## Decisions
- **Q1 鈥?[DECIDED]** keep the name `device_service` (per-io_context binding; distinct from the
  process-wide `device_manager` = discovery).
- **Q2 鈥?[DECIDED: option C]** drop `io_completion_service::is_initialized()` + `initialized_`;
  `initialize()` self-guards on `cq_`; the single "use_device called?" predicate is
  `device_service::is_registered()`.
- **Q3 鈥?[DECIDED]** `derive_effective_config` runs in `use_device` (atomic: init CQ first,
  register device only on success); `device_service` just stores the derived result.
- **Cross-cutting 鈥?[DECIDED]** service鈫抯ervice deps use a cached reference (member +
  ctor-init-list `use_service`), never runtime `use_service` (see *Cached service references*).
