# RDMA Error Code Unification Plan

## Implementation Status

Implemented in the current working tree:

- Added shared `asio::rdma::rdma_errc` / `rdma_error_category` in
  `include/rdma/rdma_error.hpp`.
- Removed backend-specific public library errors from `nd_errc` and `ibv_errc`:
  ND now keeps native `HRESULT` values only, and IBV now keeps errno/system
  helpers only.
- Removed public `nd_errc::pending`; ND async initiation paths keep the native
  `HRESULT` and compare `hr == ND_PENDING` internally.
- Migrated ND and IBV library-level conditions to shared `rdma_errc`, with
  `asio::error::operation_aborted` and `asio::error::already_open` used where
  those existing categories are more precise.
- Added `tests/rdma/test_rdma_error.cpp` and registered it with CTest.

## Goal

Unify the library-level error codes exposed by the ND and IBV backends behind a
single portable `rdma_errc` enum and `rdma_error_category`.

The project is still in a stage where breaking API changes are acceptable, so
the target design should prefer a clean public surface over preserving the
existing backend-specific `ext_*` spelling. The implementation can still be
staged so each step remains buildable and testable.

## Current State

The public API currently looks portable, but the error model is only partially
portable:

- `include/rdma/rdma_types.hpp` defines `rdma_errc` as a backend alias:
  - ND build: `using rdma_errc = nd_errc`
  - IBV build: `using rdma_errc = ibv_errc`
- ND uses one `nd_error_category` for both:
  - native NetworkDirect `HRESULT` status values such as `ND_PENDING`,
    `ND_CANCELED`, `ND_CONNECTION_REFUSED`
  - library-defined `NDEXT_*` errors such as `ext_connector_terminal`
- IBV uses:
  - `ibv_error_category` for library-defined `IBVEXT_*` errors
  - `std::system_category()` for native verbs / rdma_cm `errno`

This means source can often write `rdma_errc::ext_too_many_sge`, but the actual
enum type, numeric values, and error category are backend-specific.

## Design Decision

Use one `asio::error_code` return type everywhere, but choose the category by
where the error originates. The priority order is:

1. **Asio / standard errors when they already express the condition**
   - Examples: `asio::error::operation_aborted`, `asio::error::already_open`,
     `std::errc::invalid_argument`,
     `std::errc::not_enough_memory`, `std::errc::address_not_available`
   - These should not be duplicated as RDMA-specific enum values.
   - If an error is produced by Asio mechanics, cancellation machinery, generic
     argument validation, allocation, or address parsing, prefer the existing
     Asio/system category.

2. **Portable RDMA library semantic errors**
   - Exposed as `asio::rdma::rdma_errc`
   - Use one `rdma_error_category`
   - Returned for all cross-backend library invariants and state-machine
     decisions that do not already have an accurate Asio/system error

3. **Backend native diagnostic errors**
   - ND native `HRESULT` values remain represented by `nd_errc` /
     `nd_error_category`
   - IBV native failures remain represented by `std::system_category(errno)`
   - These are not forced into `rdma_errc` unless the library can provide a
     precise, backend-independent semantic meaning

This keeps portable user code simple while preserving native detail where it is
actually useful for debugging.

### Selection Examples

| Situation | Preferred error |
|---|---|
| Per-operation cancellation wins | `asio::error::operation_aborted` |
| RDMA object/native handle is invalid, unopened, unbound, or moved-from | `rdma_errc::invalid_handle` |
| Duplicate `open()` or duplicate registration | `asio::error::already_open` for object open; `rdma_errc::already_registered` for `use_device` registration |
| `use_device` was never called | `rdma_errc::device_not_registered` |
| A connector became single-use terminal after failed/cancelled connect | `rdma_errc::connector_terminal` |
| SGL exceeds configured device SGE cap before posting | `rdma_errc::too_many_sge` |
| Outgoing CM private data exceeds wire cap | `rdma_errc::private_data_too_large` |
| ND `IND2*` call returns raw `ND_INVALID_PARAMETER` | native `nd_errc::invalid_parameter` unless the call site can give a better portable semantic |
| IBV / rdma_cm call fails with `errno` | `std::system_category(errno)` |

The intent is to avoid growing `rdma_errc` into a shadow copy of Asio, POSIX, or
NetworkDirect.

## Proposed Public API

Add `include/rdma/rdma_error.hpp`.

```cpp
namespace asio::rdma {

enum class rdma_errc : int {
  no_available_device,

  invalid_device,
  invalid_handle,

  already_registered,
  device_not_registered,

  disconnected,
  device_removed,
  connector_terminal,

  too_many_sge,
  private_data_too_large,
};

std::error_category const& get_rdma_error_category();
asio::error_code make_error_code(rdma_errc e);

}

namespace std {
template <>
struct is_error_code_enum<asio::rdma::rdma_errc> : true_type {};
}
```

`make_error_code(rdma_errc)` must be declared in `namespace asio::rdma` so ADL
finds it. `std::is_error_code_enum<rdma_errc>` enables the standard implicit
conversion path from enum to `asio::error_code` / `std::error_code`.

Expected user spelling:

```cpp
asio::error_code ec = asio::rdma::rdma_errc::too_many_sge;
ec = make_error_code(asio::rdma::rdma_errc::connector_terminal); // ADL-supported

if (ec == asio::rdma::rdma_errc::connector_terminal) {
  // create a fresh connector
}

if (ec == asio::rdma::rdma_errc::too_many_sge) {
  // reduce the buffer sequence length
}
```

### Success Semantics

Do not add `rdma_errc::success`.

Successful completion should remain the empty/default `asio::error_code{}` and
user code should test it with `!ec`. A `{rdma_error_category, 0}` success value
would be easy to misuse because `std::error_code` equality includes the
category, so it can behave differently from a default-constructed success code.

Correct:

```cpp
if (!ec) {
  // success
}
```

Avoid:

```cpp
if (ec == rdma_errc::success) {
  // do not introduce this spelling
}
```

The same pattern should remain available for native ND errors:

```cpp
asio::error_code ec = asio::rdma::nd_errc::canceled;
ec = make_error_code(asio::rdma::nd_errc::connection_refused);
```

IBV should not need a backend semantic enum after cleanup unless a real
IBV-native, non-`errno` diagnostic category is introduced later.

### Message and Value Policy

`rdma_errc` messages should be backend-neutral and should not include `ND_EXT`
or `IBV_EXT` prefixes. The message is user-facing library semantics, not a
backend diagnostic.

Suggested messages:

| Enum | Message |
|---|---|
| `no_available_device` | `RDMA no available device` |
| `invalid_device` | `RDMA invalid device` |
| `invalid_handle` | `RDMA invalid object handle` |
| `already_registered` | `RDMA device already registered on this execution context` |
| `device_not_registered` | `RDMA device not registered (call use_device first)` |
| `disconnected` | `RDMA connection disconnected` |
| `device_removed` | `RDMA local device removed` |
| `connector_terminal` | `RDMA connector is terminal; create a new connector` |
| `too_many_sge` | `RDMA scatter/gather list exceeds device max_sge` |
| `private_data_too_large` | `RDMA outgoing private_data exceeds the CM cap` |

Numeric enum values are not a public compatibility contract. User code should
compare `asio::error_code` against enum values, not persist or branch on
`ec.value()`.

## Error Composition Model

`rdma_errc` does not physically contain ND `HRESULT` values or IBV `errno`
values. Instead, `asio::error_code` is the composition point:

```text
asio::error_code
  = { rdma_error_category, rdma_errc value }
  = { nd_error_category, native ND HRESULT value }
  = { std::system_category, native errno / std::errc value }
  = { asio::error category, Asio-defined value }
```

Every async completion still returns one `asio::error_code`, but the category
tells the caller which layer produced it. Portable user code compares against
`rdma_errc` only for library semantics. Backend-aware diagnostics can still
inspect `ec.category()` and `ec.value()` or compare against native backend
errors.

### Canonicalization at Backend Boundaries

Backend code should canonicalize native events into `rdma_errc` only when the
library is intentionally presenting a backend-independent semantic.

Examples:

- IBV `RDMA_CM_EVENT_DISCONNECTED` -> `rdma_errc::disconnected`
- IBV `RDMA_CM_EVENT_DEVICE_REMOVAL` -> `rdma_errc::device_removed`
- ND successful `NotifyDisconnect` completion -> `rdma_errc::disconnected`
- ND raw `ND_DEVICE_REMOVED` in a context that represents local device removal
  -> `rdma_errc::device_removed`
- A failed ND `CreateQueuePair` returning `ND_INVALID_PARAMETER` ->
  native `nd_errc::invalid_parameter`, because it is a native diagnostic rather
  than a library semantic
- A failed `rdma_create_id` / `ibv_create_cq` with `errno` ->
  `std::system_category(errno)`

This means the mapping is contextual, not global. Do not write a blanket
function that maps every `HRESULT` or `errno` to `rdma_errc`; doing so would
lose useful backend diagnostics and create false equivalences.

Suggested internal helpers:

```cpp
// Shared semantic constructors.
inline asio::error_code make_error_code(rdma_errc e);

// ND native constructor. Keeps raw HRESULT diagnostics.
inline asio::error_code make_nd_error_code(HRESULT hr);

// IBV native constructor. Captures errno immediately after a failing call.
inline asio::error_code last_system_error();

// Contextual adapters at semantic boundaries.
asio::error_code map_nd_disconnect_result(HRESULT hr);
asio::error_code map_ibv_cm_event(native_cm_event_t const& ev);
```

The adapters should be small and named after the semantic boundary, not after a
generic backend-to-rdma conversion.

### Device Removal Policy

Use `rdma_errc::device_removed` only when the library is surfacing a
backend-independent device-removal event/state to the user.

- IBV: `RDMA_CM_EVENT_DEVICE_REMOVAL` maps to `rdma_errc::device_removed`.
- ND: if a public completion/notification path observes `ND_DEVICE_REMOVED` as
  local device removal, map it to `rdma_errc::device_removed`.
- Otherwise, keep raw `ND_DEVICE_REMOVED` as native `nd_errc::device_removed`
  for diagnostic reporting.

Do not manufacture `device_removed` for ordinary disconnect, cancellation, or
connection-aborted paths.

## Mapping Table

| Current ND | Current IBV | New result |
|---|---|---|
| `nd_errc::ext_no_available_address` | n/a | `std::errc::address_not_available` when it is address selection; otherwise native diagnostic |
| `nd_errc::ext_no_available_provider` | n/a | `rdma_errc::no_available_device` |
| n/a | `ibv_errc::ext_no_available_device` | `rdma_errc::no_available_device` |
| `nd_errc::ext_invalid_device` | `ibv_errc::ext_invalid_device` | `rdma_errc::invalid_device` |
| `nd_errc::ext_invalid_listener` | n/a | `rdma_errc::invalid_handle` |
| `nd_errc::ext_invalid_connector` | n/a | `rdma_errc::invalid_handle` |
| `nd_errc::ext_invalid_qp` | n/a | `rdma_errc::invalid_handle` |
| `nd_errc::ext_invalid_cq` | n/a | `rdma_errc::invalid_handle` if a use site remains; otherwise delete |
| `nd_errc::ext_invalid_mr` | n/a | `rdma_errc::invalid_handle` for moved-from/unregistered MR access |
| `nd_errc::ext_already_registered` | `ibv_errc::ext_already_registered` | `rdma_errc::already_registered` |
| `nd_errc::ext_device_not_registered` | `ibv_errc::ext_device_not_registered` | `rdma_errc::device_not_registered` |
| `nd_errc::ext_no_executor` | n/a | delete if unused; otherwise prefer `asio::error::operation_not_supported` or another precise existing error after call-site review |
| `nd_errc::ext_disconnected` | `ibv_errc::ext_disconnected` | `rdma_errc::disconnected` |
| n/a or native `ND_DEVICE_REMOVED` | `ibv_errc::ext_device_removed` | `rdma_errc::device_removed` |
| `nd_errc::ext_connector_terminal` | `ibv_errc::ext_connector_terminal` | `rdma_errc::connector_terminal` |
| `nd_errc::ext_too_many_sge` | `ibv_errc::ext_too_many_sge` | `rdma_errc::too_many_sge` |
| `nd_errc::ext_private_data_too_large` | `ibv_errc::ext_private_data_too_large` | `rdma_errc::private_data_too_large` |

Open question: whether `nd_errc::ext_already_stopt` should be deleted outright.
It is currently used in an unexpected `nd_connect_op` state path and does not
look like a portable RDMA semantic. Prefer `asio::error::operation_aborted` if
the operation was cancelled/stopped, `rdma_errc::invalid_handle` if the RDMA
object/native handle state is invalid, or another more precise `rdma_errc` only
if review finds a real RDMA-layer invariant.

## Native Error Policy

Keep the following backend-native values backend-specific:

- ND:
  - `ND_CANCELED`
  - `ND_TIMEOUT`
  - `ND_CONNECTION_REFUSED`
  - `ND_CONNECTION_ABORTED`
  - `ND_DEVICE_BUSY`
  - other `HRESULT`/NetworkDirect status values
- IBV:
  - `errno` values from verbs / rdma_cm calls, via `std::system_category()`

Do not map these to `rdma_errc` merely because they look similar to socket or
RDMA concepts. A native error should become `rdma_errc` only when the library is
intentionally abstracting a cross-backend state or invariant.

### `ND_PENDING` Policy

`ND_PENDING` is not an error and should not be part of public `nd_errc`.

It is a NetworkDirect initiation status meaning the OVERLAPPED operation has
been accepted and will complete later. It should remain an internal control
result, handled by comparing the native `HRESULT` directly:

```cpp
HRESULT hr = connector->Connect(...);
if (hr == ND_PENDING) {
  scheduler.on_pending(op);
}
```

Do not complete user handlers with `ND_PENDING`, and do not ask users to compare
against `nd_errc::pending`.

Implementation cleanup:

- remove `nd_errc::pending` from `nd_error.hpp`;
- remove the `ND_PENDING` branch from the public error message switch;
- replace internal `ec == nd_errc::pending` checks with direct native
  `hr == ND_PENDING` checks;
- keep `ND_PENDING` only in ND detail/native operation helpers.

## Branching vs Diagnostic Errors

The unified design should be driven by how users naturally handle errors after
a callback or `co_await`:

```cpp
auto [ec] = co_await conn.async_connect(qp, ep, req, token);
if (ec == rdma_errc::connector_terminal) {
  // create a fresh connector
} else if (ec == asio::error::operation_aborted) {
  // cancellation / teardown won
} else if (ec) {
  // log ec.message()
}
```

Not every printable error deserves a portable enum value. Classify errors by
whether users are expected to branch on them.

### IBV Current Enum

`ibv_errc` is currently not a native verbs enum. Every value is a library-defined
`IBVEXT_*` semantic error:

- `ibv_errc::ext_no_available_device`
- `ibv_errc::ext_invalid_device`
- `ibv_errc::ext_already_registered`
- `ibv_errc::ext_device_not_registered`
- `ibv_errc::ext_disconnected`
- `ibv_errc::ext_device_removed`
- `ibv_errc::ext_connector_terminal`
- `ibv_errc::ext_too_many_sge`
- `ibv_errc::ext_private_data_too_large`

These are all reasonable callback/co_await branching targets today. Therefore
they should move to `rdma_errc` rather than be treated as mere message-printing
diagnostics.

IBV native failures are already represented separately through
`std::system_category(errno)`. Users may still branch on those, but via
Asio/system meanings such as `asio::error::connection_refused`,
`asio::error::host_unreachable`, or `std::errc::invalid_argument`, not through
`ibv_errc`.

### ND Current Enum

`nd_errc` currently mixes two different concepts:

1. native NetworkDirect `HRESULT` statuses
2. library-defined `NDEXT_*` semantic errors

Not every `NDEXT_*` entry should survive as `rdma_errc`. After review, the
portable branching candidates are:

- `nd_errc::ext_invalid_device`
- `nd_errc::ext_already_registered`
- `nd_errc::ext_device_not_registered`
- `nd_errc::ext_disconnected`
- `nd_errc::ext_connector_terminal`
- `nd_errc::ext_too_many_sge`
- `nd_errc::ext_private_data_too_large`

The following ND-only `NDEXT_*` entries should be avoided or merged:

- `nd_errc::ext_no_available_provider`
  - Merge into `rdma_errc::no_available_device`.
  - Provider is a Windows/ND implementation detail; user recovery is the same
    as "no usable RDMA device/backend found".
- `nd_errc::ext_no_available_address`
  - Prefer `std::errc::address_not_available` where the failure is generic
    address selection.
  - Do not add a portable RDMA enum unless both backends expose a distinct RDMA
    address-discovery semantic later.
- `nd_errc::ext_invalid_listener`
  - Replace with `rdma_errc::invalid_handle` for bind/listen/get operations on
    an unopened listener.
  - This avoids leaking file-descriptor terminology into RDMA object handles.
- `nd_errc::ext_invalid_connector`
  - Replace with `rdma_errc::invalid_handle` for unopened/invalid connector
    objects or invalid adopted handles.
- `nd_errc::ext_invalid_qp`
  - Replace with `rdma_errc::invalid_handle` when async connect/accept is
    given an unbound/default/moved-from queue pair.
  - IBV currently has no matching explicit enum; the user action is "bind/create
    a valid QP".
- `nd_errc::ext_invalid_cq`
  - No active cross-backend use found. Delete unless a concrete CQ operation
    needs it; prefer `rdma_errc::invalid_handle` for invalid CQ handles.
- `nd_errc::ext_invalid_mr`
  - Replace moved-from/unregistered MR access with `rdma_errc::invalid_handle`.
  - Constructor-time null/invalid device remains `rdma_errc::invalid_device`.
- `nd_errc::ext_no_executor`
  - No active use found. Delete unless a concrete public path needs it; if such
    a path appears, prefer an existing Asio/system error first.
- `nd_errc::ext_already_stopt`
  - Delete/replace as discussed above.

Some native ND statuses may still be useful for backend-specific branching, but
they should remain native diagnostics instead of becoming the primary portable
API:

- `nd_errc::connection_refused`
- `nd_errc::network_unreachable`
- `nd_errc::host_unreachable`
- `nd_errc::connection_aborted`
- `nd_errc::timeout`
- `nd_errc::io_timeout`
- `nd_errc::device_removed`
- `nd_errc::invalid_address`
- `nd_errc::device_busy`
- `nd_errc::insufficient_resources`
- `nd_errc::no_memory`
- `nd_errc::not_supported`

Other native ND statuses are mostly diagnostic/logging detail. Users can print
`ec.message()` and rebuild/fail the operation, but portable business logic
should not normally branch on them:

- `nd_errc::invalid_parameter_1` ... `nd_errc::invalid_parameter_10`
- `nd_errc::invalid_parameter_mix`
- `nd_errc::invalid_device_request`
- `nd_errc::invalid_device_state`
- `nd_errc::internal_error`
- `nd_errc::remote_error`
- `nd_errc::data_overrun`
- `nd_errc::sharing_violation`
- `nd_errc::invalid_buffer_size`
- `nd_errc::too_many_addresses`
- `nd_errc::address_already_exists`

`nd_errc::canceled` should not be a normal user branching target for this
library. Public cancellation paths should complete with
`asio::error::operation_aborted`, matching the IBV side and Asio conventions.

`nd_errc::pending` should be removed entirely. `ND_PENDING` is an internal
native initiation result, not a printable/user-branching error.

### Classification Rule

- If users need portable recovery logic, expose it as `rdma_errc`.
- If Asio or the standard library already has the exact condition, reuse that.
- If only backend-aware code can make sense of it, keep it in the backend native
  category.
- If it is provider/debug detail, rely on `ec.message()` and do not promote it
  to a portable enum.

## Revised `rdma_errc` Membership Review

The shared enum should not be the union of all backend enums. It should contain
only stable, portable recovery semantics:

| Proposed member | Keep? | Rationale |
|---|---:|---|
| `success` | no | Success is represented by `asio::error_code{}` / `!ec`, not by a category-specific enum value. |
| `no_available_device` | yes | Both backends can fail discovery because no usable RDMA device/backend exists; ND provider absence maps here. |
| `invalid_device` | yes | Both ND and IBV use this for null/invalid RDMA device handles and missing device internals. |
| `invalid_handle` | yes | Unifies invalid/unopened/unbound/moved-from RDMA object handles without borrowing socket/file-descriptor terminology. Replaces ND-only `invalid_listener` / `invalid_connector` / `invalid_qp` / `invalid_cq` / `invalid_mr`, and gives IBV matching public semantics where it currently uses `bad_descriptor`. |
| `already_registered` | yes | Both backends use this for duplicate `use_device` / completion service registration. Duplicate object `open()` should still prefer `asio::error::already_open`. |
| `device_not_registered` | yes | Both backends rely on `use_device` registration; this is a common user-actionable setup error. |
| `disconnected` | yes | Both backends expose peer/self disconnect notification as a library semantic event. |
| `device_removed` | yes | IBV has CM device-removal; ND can map native device-removal contexts here when used as a library-level event. |
| `connector_terminal` | yes | Both backends treat connectors as one-shot after failed/cancelled/closed connection attempts; user must create a fresh connector. ND already applies this to connect and accept. IBV applies it to connect today; accept should be brought to the same public guard. |
| `too_many_sge` | yes | Both backends validate SGE count before posting; user can retry with fewer segments. |
| `private_data_too_large` | yes | Both backends share the outgoing CM private-data cap and can reject before connect/accept. |

Members reviewed and rejected:

| Rejected member | Replacement |
|---|---|
| `no_available_address` | `std::errc::address_not_available` |
| `no_available_provider` | `rdma_errc::no_available_device` |
| `invalid_listener` | `rdma_errc::invalid_handle` |
| `invalid_connector` | `rdma_errc::invalid_handle` |
| `invalid_qp` | `rdma_errc::invalid_handle` |
| `invalid_cq` | `rdma_errc::invalid_handle` or delete if no use site remains |
| `invalid_mr` | `rdma_errc::invalid_handle`; constructor invalid device remains `rdma_errc::invalid_device` |
| `no_executor` | delete if unused; otherwise existing Asio/system error after call-site review |

### `connector_terminal` Coverage

`rdma_errc::connector_terminal` should mean:

- the connector's connection-management object is no longer fresh/reusable;
- retrying connect/accept on this connector would touch a stranded one-shot
  native CM object;
- user recovery is to construct/open/assign a fresh connector.

Current coverage:

- ND:
  - `async_connect`: checks `connect_state != idle` and completes with
    `ext_connector_terminal`.
  - `async_accept`: checks `connect_state != idle` and completes with
    `ext_connector_terminal`.
- IBV:
  - `async_connect`: checks `connect_state != idle` and completes with
    `ext_connector_terminal`.
  - `async_accept`: currently relies on the later `idle -> connecting` CAS in
    `start_accept_op`; CAS failure completes as `operation_aborted`.

Required cleanup:

- Add the same public `connect_state != idle -> rdma_errc::connector_terminal`
  guard to IBV `async_accept` before starting the op.
- Keep `operation_aborted` for true cancellation races where an already-started
  operation loses to `disconnect()` / per-op cancellation.
- Add/adjust tests so reused terminal connectors are rejected with
  `rdma_errc::connector_terminal` on both connect and accept paths where the
  public API can observe such reuse.

## Reuse Existing Asio/System Errors

Before adding or returning a new `rdma_errc`, check whether an existing
Asio/system error already communicates the condition:

- Cancellation:
  - use `asio::error::operation_aborted`
  - do not add `rdma_errc::cancelled`
- Generic bad state matching Asio-owned descriptors:
  - use `asio::error::bad_descriptor` only for actual Asio descriptor-style
    resources where that category is the clearest fit
  - use `rdma_errc::invalid_handle` for invalid RDMA objects/native handles
    such as listener/connector/QP/CQ/MR wrappers
  - use `asio::error::already_open` for duplicate `open()` if the object models
    socket/acceptor behavior
- Generic invalid arguments / allocation:
  - use `std::errc::invalid_argument`
  - use `std::errc::not_enough_memory`
- Address parsing / local address availability:
  - use `std::errc::address_not_available` when this is a generic address
    selection failure

Use `rdma_errc` when the condition is specific to this RDMA abstraction and
should be portable across ND and IBV.

## Ambiguous Current Codes: Call-Site Rules

Some existing backend `ext_*` codes are overloaded today and should not be
renamed mechanically.

### `ext_already_registered`

Use the new result based on what is being duplicated:

- duplicate `use_device()` / per-`io_context` device or CQ registration:
  `rdma_errc::already_registered`
- duplicate `connector.open()` / `listener.open()`:
  `asio::error::already_open`

Current IBV code uses `ibv_errc::ext_already_registered` for both categories;
the implementation pass should split these call sites.

### `ext_invalid_device`

Use the new result based on what is invalid:

- null/invalid `rdma_device_ptr`, missing PD/context, or invalid registered
  device internals: `rdma_errc::invalid_device`
- invalid connector/listener/QP/CQ/MR object or adopted native handle:
  `rdma_errc::invalid_handle`

Current IBV `connector.assign()` uses `ext_invalid_device` for an invalid
adopted connector handle; that should become `rdma_errc::invalid_handle`, not
`rdma_errc::invalid_device`.

### `ext_no_available_*`

Use the new result based on user recovery:

- no usable RDMA device/backend/provider: `rdma_errc::no_available_device`
- generic local address selection failure: `std::errc::address_not_available`

`no_available_provider` is an ND implementation detail and should not survive
as a public portable enum.

## Call-Site Audit Checklist

The implementation pass should explicitly audit these non-mechanical mappings.

IBV:

- `connector.open()` duplicate open:
  `ibv_errc::ext_already_registered` -> `asio::error::already_open`
- `listener.open()` duplicate open:
  `ibv_errc::ext_already_registered` -> `asio::error::already_open`
- duplicate `use_device()` / IO completion registration:
  `ibv_errc::ext_already_registered` -> `rdma_errc::already_registered`
- `connector.assign()` with an empty/invalid adopted handle:
  `ibv_errc::ext_invalid_device` -> `rdma_errc::invalid_handle`
- invalid/null `rdma_device_ptr` and missing PD/context:
  `ibv_errc::ext_invalid_device` -> `rdma_errc::invalid_device`
- `listener.async_get_connection()` or `connector.async_wait_disconnect()` on an
  unopened object:
  current `asio::error::bad_descriptor` -> `rdma_errc::invalid_handle`
- `async_accept()` on a non-idle connector:
  add public guard -> `rdma_errc::connector_terminal`

ND:

- invalid listener/connector/QP/CQ/MR wrapper:
  `nd_errc::ext_invalid_*` -> `rdma_errc::invalid_handle`
- null/invalid device:
  `nd_errc::ext_invalid_device` -> `rdma_errc::invalid_device`
- provider absence:
  `nd_errc::ext_no_available_provider` -> `rdma_errc::no_available_device`
- address selection failure:
  `nd_errc::ext_no_available_address` -> `std::errc::address_not_available`
- `nd_errc::ext_already_stopt`:
  delete and replace per reviewed path (`operation_aborted`,
  `invalid_handle`, or a more specific existing error)
- `nd_errc::pending` / `ND_PENDING`:
  remove from public `nd_errc`; compare the native `HRESULT` directly
  (`hr == ND_PENDING`) before converting to `asio::error_code`

## Implementation Plan

### Phase 1: Add the Shared Error Layer

- Add `include/rdma/rdma_error.hpp`.
- Include it from `include/rdma/rdma.hpp` and `include/rdma/rdma_types.hpp`.
- Replace the backend alias in `rdma_types.hpp`:
  - remove `using rdma_errc = nd_errc`
  - remove `using rdma_errc = ibv_errc`
- Add `tests/rdma/test_rdma_error.cpp`:
  - category name is stable, e.g. `"rdma_error_code"`
  - `make_error_code(rdma_errc::too_many_sge)` has the shared category
  - messages are backend-neutral and do not contain `ND_EXT` / `IBV_EXT`
  - representative messages are stable
  - `ec == rdma_errc::connector_terminal` works
  - `ec == rdma_errc::invalid_handle` works
  - `asio::error_code ec = rdma_errc::too_many_sge` compiles
  - unqualified `make_error_code(rdma_errc::too_many_sge)` finds the ADL
    overload
  - there is no `rdma_errc::success`; successful operations are tested with
    `!ec`

### Phase 2: Move Library-Level Returns to `rdma_errc`

Update both backends so all library semantic errors return the shared enum:

- `use_device`
  - already registered
  - invalid device
- device discovery
  - no available device/backend/provider -> `rdma_errc::no_available_device`
  - address selection failures -> `std::errc::address_not_available`
- service guards
  - device not registered
  - invalid listener / connector / QP / CQ / MR ->
    `rdma_errc::invalid_handle`
- connect/accept state machine
  - disconnected
  - connector terminal
  - private data too large
- data plane validation
  - too many SGE

At each call site, first apply the reuse rule above. For example, a cancelled
operation should complete with `asio::error::operation_aborted`, not
`rdma_errc::disconnected`; a duplicate object `open()` should keep matching
Asio object semantics where practical.

Representative code change:

```cpp
ec = rdma_errc::device_not_registered;
```

or:

```cpp
p.p->ec_ = make_error_code(rdma_errc::connector_terminal);
```

Prefer direct assignment where `asio::error_code` supports ADL
`make_error_code`.

For native backend errors, keep the existing native constructor style:

```cpp
ec = make_nd_error_code(hr);       // ND raw HRESULT
ec = last_system_error();          // IBV errno
```

Only contextual semantic boundaries should convert native events to
`rdma_errc`.

### Phase 3: Clean Backend Error Headers

ND:

- Keep `nd_errc` for native ND status values.
- Remove `ND_PENDING` from `nd_errc`; it is an internal initiation status, not a
  user-visible error.
- Remove `NDEXT_*` macros and `nd_errc::ext_*` entries after all call sites are
  migrated.
- Keep `make_nd_error_code(HRESULT)` and `make_error_code(nd_errc)` for native
  ND diagnostics.
- Keep `std::is_error_code_enum<nd_errc>` so `asio::error_code ec =
  nd_errc::canceled` continues to work for native ND code.
- Keep tests that assert native messages such as `ND_SUCCESS` and
  `ND_CANCELED`.

IBV:

- Delete `ibv_errc` if all entries were library-level `ext_*`.
- Delete `ibv_error_category`, `get_ibv_error_category()`, and
  `make_ibv_error_code()` if unused.
- Keep `last_system_error()` / `make_system_error_code()` if convenient, but
  consider moving them into a shared or detail namespace to avoid implying an
  IBV-specific semantic error category.

### Phase 4: Update Public Docs and Tests

Update user-facing docs:

- `README.md`
  - `rdma_errc::ext_too_many_sge` ->
    `rdma_errc::too_many_sge`
  - `rdma_errc::ext_private_data_too_large` ->
    `rdma_errc::private_data_too_large`
  - `ext_connector_terminal` ->
    `rdma_errc::connector_terminal`
- `CLAUDE.md`
  - document the two-layer error model
- design docs that describe current behavior:
  - `docs/sgl_buffer_plan.md`
  - `docs/disconnect_refactor_plan.md`
  - `docs/connect_private_data_plan.md`
  - cancellation docs

Update tests:

- Backend-independent semantic assertions should use `rdma::rdma_errc`.
- Backend-native assertions should remain backend-specific.
- Generic cancellation assertions should use `asio::error::operation_aborted`.
- Invalid RDMA object/handle assertions should use
  `rdma::rdma_errc::invalid_handle`.
- Duplicate object `open()` assertions should use `asio::error::already_open`;
  duplicate `use_device()` assertions should use
  `rdma::rdma_errc::already_registered`.
- ND tests currently checking `rdma::nd_errc::ext_*` should move to
  `rdma::rdma_errc::*`.
- IBV tests currently checking `rdma::ibv_errc::ext_*` should move to
  `rdma::rdma_errc::*`.

### Phase 5: Final Dead-Code Sweep

Run searches and remove stale references:

```sh
rg "ext_|NDEXT_|IBVEXT_|ibv_errc|nd_errc::ext|rdma_errc::ext" include tests docs README.md CLAUDE.md
```

Expected final state:

- No `rdma_errc::ext_*`
- No `ibv_errc::ext_*`
- No `nd_errc::ext_*`
- `nd_errc` remains only for native ND status values
- `ibv_errc` is gone unless a new backend-native IBV semantic enum is
  deliberately introduced
- Cancellations that are not native backend statuses use
  `asio::error::operation_aborted`

## Acceptance Criteria

The refactor is complete only when all of the following are true:

- Public portable semantic errors use `rdma_errc` and `rdma_error_category`.
- `rdma_types.hpp` no longer aliases `rdma_errc` to `nd_errc` or `ibv_errc`.
- `rdma_errc` contains no `success` value; success is represented by
  `asio::error_code{}` / `!ec`.
- `rdma_errc` contains `invalid_handle`, not the object-specific
  `invalid_listener` / `invalid_connector` / `invalid_qp` / `invalid_cq` /
  `invalid_mr` values.
- `ND_PENDING` is removed from public `nd_errc` and never reaches a user
  completion handler.
- Internal ND pending checks compare native `HRESULT` directly with
  `ND_PENDING` before converting failures to `asio::error_code`.
- `ibv_errc` and `ibv_error_category` are removed unless a real IBV-native,
  non-`errno` diagnostic category is introduced.
- `ND_EXT` / `IBV_EXT` prefixes do not appear in shared `rdma_errc` messages.
- Duplicate object `open()` uses `asio::error::already_open`.
- Duplicate device/CQ registration uses `rdma_errc::already_registered`.
- Invalid RDMA object/native handle paths use `rdma_errc::invalid_handle`.
- Invalid RDMA device paths use `rdma_errc::invalid_device`.
- Both ND and IBV reject terminal connector reuse with
  `rdma_errc::connector_terminal`; IBV `async_accept` has the same public guard
  as `async_connect`.
- Cancellation paths that are not native backend diagnostics use
  `asio::error::operation_aborted`.
- Backend-native ND statuses remain available through `nd_errc` for
  backend-aware diagnostics.
- IBV native failures still use `std::system_category(errno)`.
- Tests cover ADL `make_error_code(rdma_errc)`, implicit enum-to-error-code
  conversion, category identity, stable backend-neutral messages, and key
  branch comparisons.

## Files Likely to Change

Shared:

- `include/rdma/rdma_error.hpp` (new)
- `include/rdma/rdma.hpp`
- `include/rdma/rdma_types.hpp`
- `README.md`
- `CLAUDE.md`

ND:

- `include/nd/nd_error.hpp`
- `include/nd/nd_use_device.hpp`
- `include/nd/nd_mr.hpp`
- `include/nd/nd_queue_pair.hpp`
- `include/nd/nd_completion_queue.hpp`
- `include/nd/detail/nd_device_impl.hpp`
- `include/nd/detail/nd_ops_cm.hpp`
- `include/nd/detail/nd_ops_verbs.hpp`
- `include/nd/detail/nd_op_connect.hpp`
- `include/nd/detail/nd_op_wait_disconnect.hpp`
- `include/nd/detail/nd_service_connector.hpp`
- `include/nd/detail/nd_service_listener.hpp`
- `include/nd/detail/nd_service_io_completion.hpp`
- `include/nd/detail/nd_service_verbs.hpp`

IBV:

- `include/ibv/ibv_error.hpp`
- `include/ibv/ibv_use_device.hpp`
- `include/ibv/ibv_mr.hpp`
- `include/ibv/ibv_queue_pair.hpp`
- `include/ibv/detail/ibv_device_impl.hpp`
- `include/ibv/detail/ibv_op_connect.hpp`
- `include/ibv/detail/ibv_op_wait_disconnect.hpp`
- `include/ibv/detail/ibv_service_connector.hpp`
- `include/ibv/detail/ibv_service_listener.hpp`
- `include/ibv/detail/ibv_service_io_completion.hpp`
- `include/ibv/detail/ibv_service_verbs.hpp`

Tests:

- `tests/rdma/CMakeLists.txt`
- `tests/rdma/test_rdma_error.cpp` (new)
- `tests/rdma/test_rdma_sgl.cpp`
- `tests/nd/test_nd_error.cpp`
- `tests/nd/test_nd_*cancel*.cpp`
- `tests/nd/test_nd_connector_listener.cpp`
- `tests/nd/test_nd_wait_disconnect.cpp`
- `tests/ibv/test_ibv_*cancel*.cpp`
- `tests/ibv/test_ibv_wait_disconnect.cpp`

## Verification Plan

Build:

```sh
cmake --build build --config Debug
```

CTest:

```sh
ctest --test-dir build -C Debug --output-on-failure
```

Focused Windows/ND tests:

```sh
.\build\tests\nd\Debug\test_nd_error.exe
.\build\tests\nd\Debug\test_nd_connector_listener.exe
.\build\tests\nd\Debug\test_nd_control_cancel.exe <rdma-ip> <port>
.\build\tests\nd\Debug\test_nd_wait_disconnect.exe <rdma-ip> <port>
```

Focused cross-platform tests:

```sh
.\build\tests\rdma\Debug\test_rdma_error.exe
.\build\tests\rdma\Debug\test_rdma_sgl.exe <rdma-ip> <port>
.\build\tests\rdma\Debug\test_rdma_private_data.exe <rdma-ip> <port>
```

Linux/IBV verification should run the analogous IBV targets on a Linux host:

```sh
./build/tests/ibv/test_ibv_wait_disconnect <rdma-ip> <port>
./build/tests/ibv/test_ibv_control_cancel <rdma-ip> <port>
./build/tests/ibv/test_ibv_disconnect_cancel <rdma-ip> <port>
```

## Compatibility Notes

This is intentionally a breaking cleanup:

- Public code using `rdma_errc::ext_*` must rename to non-`ext` shared names.
- Public code using `nd_errc::ext_*` or `ibv_errc::ext_*` for portable library
  semantics must switch to `rdma_errc`.
- Public code that intentionally checks ND native statuses can continue using
  `nd_errc`.
- Public code that checks IBV native failures should continue checking
  `std::system_category()` / `std::errc` style errors where appropriate.
- Public code should keep checking generic cancellation through
  `asio::error::operation_aborted`, not `rdma_errc`.

Because the project is still pre-stable, the recommended path is to perform the
full cleanup rather than preserving deprecated aliases long term. If review
finds too much churn in one patch, use the phases above as separate commits.
