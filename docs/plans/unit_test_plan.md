# Unit Test Plan

## Goals

Build a repeatable unit-test suite that covers:

- All public APIs exposed through `rdma/rdma.hpp` and backend-specific public
  headers.
- Critical `detail` helpers whose behavior is part of the library's correctness,
  even if they are not public ABI.
- Backend-neutral behavior first, then ND/IBV backend parity.
- Hardware-independent tests in CTest by default, with hardware/integration
  tests kept explicit and opt-in.
- The planned public header layout migration:
  backend-specific headers live under `include/rdma/nd/...` and
  `include/rdma/ibv/...`.

## Implementation Status

Initial execution has landed:

- `tests/unit/unit_test.hpp`, copied from Asio's unit-test harness.
- `tests/unit/CMakeLists.txt` and `RDMA_BUILD_UNIT_TESTS` CMake integration.
- Backend-neutral unit tests for error codes, RDMA buffers, TCP port space, and
  public API compile surface.
- Backend-neutral operation-object tests for send/recv/read/write op type,
  remote-address retention, completion byte counts, and error propagation.
- Backend-neutral value-type tests for `rdma_config_t`, `rdma_remote_addr_t`,
  and `completion_mode`.
- ND unit tests for config derivation, native error helpers, SGE conversion,
  public header layout, and no-hardware service guard behavior.
- ND no-hardware service guard coverage now includes default queue-pair state,
  invalid connector assignment, unopened listener bind/listen, null-device
  `use_device`, null-device memory-region construction, and immediate
  `async_connect` / `async_accept` failures before `use_device`.
- Stage 4 public API guard coverage has started:
  - throwing overloads now verify the same no-`use_device` / null-device errors
    as their `error_code&` counterparts;
  - unopened connector `disconnect` and `async_wait_disconnect` now return
    `rdma_errc::invalid_handle`;
  - unopened listener `async_get_connection` return-form and fill-form now
    complete with `rdma_errc::invalid_handle` and preserve connector state;
  - compile guards assert that backend listener `bind` accepts only a port, not
    an address/endpoint.
- IBV counterpart unit tests in the same shape, built on IBV configurations.
- Backend headers moved to `include/rdma/nd/...` and `include/rdma/ibv/...`.
- Existing hardware integration tests registered behind
  `RDMA_ENABLE_HARDWARE_TESTS` and `RDMA_TEST_ADDR` where the executable can
  run as a single process.
- Hardware-capable public API guard coverage has landed in
  `test_rdma_regression`:
  - connector/listener duplicate `open()` -> `asio::error::already_open`;
  - queue-pair default state, event bind, poll bind, and duplicate bind;
  - empty completion-queue `poll_one()` / `poll()`;
  - memory-region metadata, slices, remote addresses, invalid ranges, and
    moved-from `invalid_handle` behavior.
- Stage 6 correctness regressions have landed in `test_rdma_regression`:
  - RDMA read/write round trip;
  - zero-length send/recv immediate completion on an established QP;
  - multi-message send/recv ordering;
  - negative connect to a port with no listener.
- Default CTest registration is now limited to unit/compile/safe legacy tests.
  Hardware/provider integration tests are opt-in through
  `RDMA_ENABLE_HARDWARE_TESTS`; tests needing an address additionally require
  `RDMA_TEST_ADDR`.

Deferred from the default unit gate:

- Unopened `async_wait_disconnect` and unopened `async_get_connection` are no
  longer deferred; their immediate-failure behavior is now covered by the
  service guard tests.
- Hardware-backed Stage 4/5/6 tests are intentionally outside the default unit
  gate and run only when hardware testing is enabled.
- Two-process echo/example executables remain built but are not default CTest
  entries; single-process hardware regressions cover the correctness paths that
  can be run reliably under CTest.

The project already has useful tests, but most of them are closer to smoke or
integration tests. The next iteration should make the test suite smaller,
stricter, easier to extend, and safer to run in CI or on a developer machine
without an RDMA device.

## Review Of Existing Tests

### What Is Good

- The current tests cover real user workflows, not only type checks:
  connect/accept, send/recv echo, poll-mode CQ, private data, scatter/gather,
  disconnect, wait-disconnect, and control-plane cancellation.
- `tests/rdma` already exercises the portable public surface, which is the right
  place for cross-backend contract tests.
- The private-data and SGL tests are especially valuable because they validate
  tricky API lifetime and buffer-sequence semantics.
- `test_nd_config_derive` is a good example of a true unit test: no hardware,
  deterministic inputs, direct assertions on a `detail` helper.
- Recent error-code tests correctly separate portable `rdma_errc` from native ND
  diagnostics.

### Weaknesses To Fix

- Tests use raw `assert()`. This is terse, but failure output does not show both
  expected and actual values, and assertions disappear if `NDEBUG` is ever used.
- Test organization does not clearly distinguish:
  - pure unit tests,
  - compile/API-surface tests,
  - hardware smoke tests,
  - long-running or race-oriented integration tests.
- Many executables are built but not registered with CTest. That is fine for
  hardware tests needing an IP, but it should be explicit via labels and options.
- Skip semantics are ad hoc: returning success after printing `[SKIP]` is useful
  locally, but CTest cannot tell whether coverage was actually exercised.
- Several tests combine many behaviors in one executable. That makes failures
  slower to localize.
- The `detail` namespace is under-tested. Important pure logic such as
  `rdma_buffer` helpers and `buffers2sglist` can be tested without hardware.
- Backend parity is partly implicit. ND and IBV often have mirrored tests, but
  the coverage matrix is not written down, so regressions can slip in by only
  updating one side.

## Test Framework Recommendation

Follow Asio's own unit-test style instead of introducing doctest, Catch2, or
GoogleTest.

The vendored Asio tree uses `third_party/asio/src/tests/unit/unit_test.hpp`, a
small custom harness with:

- `ASIO_CHECK(expr)` for non-fatal checks.
- `ASIO_CHECK_MESSAGE(expr, msg)` for checks with custom diagnostics.
- `ASIO_ERROR(msg)` for unconditional failure.
- `ASIO_TEST_CASE(test)` to register a runtime test function.
- `ASIO_COMPILE_TEST_CASE(test)` to register a compile-only test.
- `ASIO_TEST_SUITE(name, tests)` to generate `main()`.

This is the best fit if the long-term goal is to upstream the library into Asio:

- No third-party test framework dependency.
- Test source style matches Asio's existing repository.
- Compile-only tests and runtime tests use the same harness.
- The harness is tiny enough that we can copy/adapt it under our own `tests/`
  tree while keeping the Boost Software License notice intact.

Migration rule:

- New unit tests should use the Asio-style `unit_test.hpp` harness.
- Existing integration tests can stay as plain executables initially.
- Do not bulk-rewrite all existing tests in one pass. Convert tests when they are
  touched or when they move into the unit-test group.

### Harness Ownership

Do not include Asio's test harness through the vendored internal path in new
tests. Although this repository currently vendors the full Asio source tree,
`third_party/asio/src/tests/unit/unit_test.hpp` is not part of Asio's public
installed headers.

Use this approach instead:

- Copy/adapt Asio's `src/tests/unit/unit_test.hpp` to
  `tests/unit/unit_test.hpp`.
- Preserve the original Boost Software License notice.
- Keep the macro names and style compatible with Asio:
  `ASIO_CHECK`, `ASIO_CHECK_MESSAGE`, `ASIO_ERROR`, `ASIO_TEST_CASE`,
  `ASIO_COMPILE_TEST_CASE`, and `ASIO_TEST_SUITE`.
- Add only minimal project-local extensions if needed. Prefer none. If a helper
  is project-specific, place it in a separate `tests/unit/rdma_test_helpers.hpp`
  instead of changing the Asio-style harness.

This keeps the current CMake build self-contained while making future upstream
alignment with Asio straightforward.

## Existing Test Classification

The current `tests/` tree should not be treated as one uniform unit-test suite.
Most files are useful, but they belong in different buckets.

### Good Example Candidates

These are close to user-facing demonstrations and can eventually be copied or
trimmed into an `examples/` tree:

- `tests/rdma/test_rdma_echo.cpp`
  - Portable event-mode echo using `rdma_*` aliases.
  - Good candidate for `examples/rdma_echo`.
- `tests/rdma/test_rdma_echo_poll.cpp`
  - Portable poll-mode echo.
  - Good candidate for `examples/rdma_echo_poll`.
- `tests/nd/test_nd_echo.cpp` and `tests/ibv/test_ibv_echo.cpp`
  - Backend-specific echo examples.
  - Useful while backend-specific APIs remain public, but less important than
    the portable `rdma_*` examples.
- `tests/nd/test_nd_echo_poll.cpp` and `tests/ibv/test_ibv_echo_poll.cpp`
  - Backend-specific poll-mode examples.

Example extraction rule:

- Examples should show the shortest happy-path workflow.
- Examples should not contain race testing, failure matrices, watchdogs, or
  detailed regression assertions.
- Prefer portable `rdma_*` examples over backend-specific examples unless the
  example is intentionally documenting backend-specific behavior.

### Unit-Test Candidates

These are deterministic or can be made deterministic. They should move toward
`tests/unit/...` and use the Asio-style harness:

- `tests/rdma/test_rdma_error.cpp`
  - Portable `rdma_errc` category, ADL, conversion, and comparison checks.
- `tests/nd/test_nd_error.cpp`
  - Native ND error category checks and `ND_PENDING` public-surface policy.
- `tests/nd/test_nd_config_derive.cpp`
  - Pure `detail::derive_effective_config` unit tests.
- `tests/nd/test_nd_connector_listener.cpp`
  - Split this file:
    - no-`use_device` guards are unit/smoke tests;
    - real listener open/bind/listen is hardware smoke.
- `tests/nd/test_nd_use_device.cpp`
  - Split this file:
    - null-device and duplicate-registration checks are unit/smoke tests;
    - real device registration is hardware smoke.
- `tests/nd/test_nd_refactored_compile.cpp`
  - Convert to compile-surface tests using `ASIO_COMPILE_TEST_CASE` where
    possible.
- `tests/nd/test_nd_device_manager.cpp` and
  `tests/ibv/test_ibv_device_manager.cpp`
  - Singleton and impossible-config checks are testable without requiring a
    successful device.
  - Successful discovery remains hardware smoke.
- `tests/ibv/test_ibv_connector_listener.cpp`
  - Same split as the ND connector/listener test.

### Integration And Regression Tests

These should stay as integration/regression tests. They need hardware, network
configuration, or timing-sensitive state-machine behavior:

- `tests/rdma/test_rdma_private_data.cpp`
  - Regression test for private-data matrix, lifetime, no-reply overloads, and
    cancellation-slot propagation.
- `tests/rdma/test_rdma_sgl.cpp`
  - Regression test for multi-MR SGL behavior and too-many-SGE rejection.
- `tests/nd/test_nd_control_cancel.cpp`
- `tests/ibv/test_ibv_control_cancel.cpp`
  - Per-operation control-plane cancellation.
- `tests/nd/test_nd_disconnect_cancel.cpp`
- `tests/ibv/test_ibv_disconnect_cancel.cpp`
  - Cross-thread disconnect and connector terminal state.
- `tests/nd/test_nd_wait_disconnect.cpp`
- `tests/ibv/test_ibv_wait_disconnect.cpp`
  - Graceful disconnect watcher and level-trigger behavior.
- `tests/ibv/test_ibv_link.cpp`
  - Environment/link smoke test for rdma-core.

Integration extraction rule:

- Keep these out of default no-hardware CTest.
- Register them behind `RDMA_ENABLE_HARDWARE_TESTS`.
- Label timing-sensitive cases with `race`.
- Keep them strict about expected error codes and terminal states, even if they
  remain plain executables for now.

## Asio Test Directory Model

The vendored Asio source tree organizes tests like this:

```text
third_party/asio/src/tests/
  Makefile.am
  unit/
    unit_test.hpp
    buffer.cpp
    error.cpp
    io_context.cpp
    basic_socket.cpp
    steady_timer.cpp
    ip/
      address.cpp
      tcp.cpp
      udp.cpp
      basic_endpoint.cpp
      basic_resolver.cpp
    execution/
      executor.cpp
      any_executor.cpp
      blocking.cpp
      mapping.cpp
    experimental/
      parallel_group.cpp
      channel.cpp
      coro/
        co_spawn.cpp
        cancel.cpp
    windows/
      basic_stream_handle.cpp
      overlapped_ptr.cpp
    posix/
    ssl/
    generic/
    local/
    ts/
    archetypes/
  properties/
    cpp03/
    cpp11/
    cpp14/
  performance/
    client.cpp
    server.cpp
    handler_allocator.hpp
  latency/
    tcp_client.cpp
    tcp_server.cpp
    udp_client.cpp
    udp_server.cpp
```

Key observations:

- `unit/` is the main test suite.
- Most public headers or features get one focused `.cpp`.
- Subdirectories map to namespaces, platforms, or feature groups.
- `unit_test.hpp` lives inside `unit/` and is included by unit test sources.
- Each `.cpp` is normally built as its own test executable.
- `properties/` is for compiler/standard/build-property checks.
- `performance/` and `latency/` are not ordinary unit tests. RDMA-specific
  stress, performance, and latency work is tracked separately in
  `docs/rdma_stress_performance_plan.md`.
- Standalone Asio does not provide a CMake test layout; its source package uses
  `Makefile.am` / platform build files to enumerate executables.

RDMA-on-Asio should follow the source layout and harness style, while using this
project's CMake files to enumerate executables and register CTest tests.

## Header Layout Migration

The unit-test cleanup should also cover the include tree migration requested for
the public headers.

Target layout:

```text
include/
  rdma/
    rdma.hpp
    rdma_types.hpp
    rdma_commons.hpp
    rdma_error.hpp
    rdma_buffer.hpp
    tcp.hpp
    detail/
      ...
    nd/
      nd_connector.hpp
      nd_listener.hpp
      nd_queue_pair.hpp
      nd_completion_queue.hpp
      nd_mr.hpp
      nd_device.hpp
      nd_use_device.hpp
      nd_error.hpp
      nd_buffer.hpp
      nd_types.hpp
      detail/
        ...
    ibv/
      ibv_connector.hpp
      ibv_listener.hpp
      ibv_queue_pair.hpp
      ibv_completion_queue.hpp
      ibv_mr.hpp
      ibv_device.hpp
      ibv_use_device.hpp
      ibv_error.hpp
      ibv_buffer.hpp
      ibv_types.hpp
      detail/
        ...
```

Rationale:

- Makes the public include root consistently `rdma/...`.
- Keeps backend-specific headers namespaced by path as well as by C++ namespace.
- Reduces the chance that `nd/...` or `ibv/...` conflicts with system or vendor
  headers.
- Better matches an upstream-Asio style where feature headers live under one
  component root.

Migration policy:

- Prefer the new include paths in all source and tests:
  - `#include "rdma/nd/nd_connector.hpp"`
  - `#include "rdma/ibv/ibv_connector.hpp"`
- Update umbrella headers first:
  - `rdma/tcp.hpp`
  - `rdma/rdma_types.hpp`
  - `rdma/rdma.hpp`
- Then update backend headers and detail headers bottom-up.
- Finally update tests and docs.
- Because this library is still in a breaking-refactor phase, compatibility
  shims are optional. If shims are added, they should be temporary forwarding
  headers only, with a removal note in the plan.

Test implications:

- Add compile-surface tests for the new include paths:
  - `#include "rdma/rdma.hpp"`
  - `#include "rdma/nd/nd_error.hpp"` on ND builds
  - `#include "rdma/ibv/ibv_error.hpp"` on IBV builds
- Add negative review rule: new tests and examples must not include
  `"nd/..."` or `"ibv/..."` after the migration.
- Keep integration tests focused on behavior; do not mix include-path migration
  with behavior changes in the same test.

## Proposed Test Layout

```text
examples/
  rdma_echo.cpp
  rdma_echo_poll.cpp

tests/
  unit/
    unit_test.hpp
    rdma/
      error.cpp
      buffer.cpp
      tcp.cpp
      public_api_compile.cpp
    nd/
      config_derive.cpp
      error.cpp
      buffer.cpp
      service_state_guards.cpp
    ibv/
      config_derive.cpp
      error_helpers.cpp
      buffer.cpp
      service_state_guards.cpp
  integration/
    rdma/
      echo.cpp
      echo_poll.cpp
      private_data.cpp
      sgl.cpp
    nd/
      control_cancel.cpp
      disconnect_cancel.cpp
      wait_disconnect.cpp
    ibv/
      control_cancel.cpp
      disconnect_cancel.cpp
      wait_disconnect.cpp
  properties/
    public_headers.cpp
    no_exceptions.cpp
```

This can be staged without moving files immediately:

- First add `tests/unit/...` for new deterministic tests.
- Keep current tests in place.
- Later move existing tests into `integration` when CMake cleanup happens.
- Add `properties/` only when there is a real test in that category; do not
  create empty directories.
- Stress, performance, and latency programs are intentionally outside this unit
  test plan. Keep their scope in `docs/rdma_stress_performance_plan.md` so the
  default correctness gate stays fast and deterministic.

## CMake And CTest Plan

Add these CMake switches:

- `RDMA_BUILD_UNIT_TESTS=ON` by default.
- `RDMA_BUILD_INTEGRATION_TESTS=ON` by default if current behavior should stay.
- `RDMA_ENABLE_HARDWARE_TESTS=OFF` by default.
- `RDMA_TEST_ADDR=<ip>` optional cache string for hardware tests.
- `RDMA_TEST_BASE_PORT=<port>` optional cache string.

Register tests with labels:

- `unit`: deterministic, no RDMA hardware, should run in every `ctest`.
- `compile`: API/compile-surface checks.
- `hardware`: needs RDMA hardware and a local RDMA IP.
- `integration`: exercises real backend behavior.
- `race`: cancellation or concurrency tests that may be timing-sensitive.
- `nd` / `ibv` / `rdma`: backend labels.

Default CTest should run only `unit` and safe `compile` tests. Hardware tests
should be enabled only when `RDMA_ENABLE_HARDWARE_TESTS=ON` and
`RDMA_TEST_ADDR` is set.

Example policy:

```cmake
add_test(NAME unit_rdma_buffer COMMAND unit_rdma_buffer)
set_tests_properties(unit_rdma_buffer PROPERTIES LABELS "unit;rdma")

if(RDMA_ENABLE_HARDWARE_TESTS AND RDMA_TEST_ADDR)
  add_test(NAME integration_rdma_sgl
           COMMAND test_rdma_sgl ${RDMA_TEST_ADDR} ${RDMA_TEST_BASE_PORT})
  set_tests_properties(integration_rdma_sgl PROPERTIES LABELS "integration;hardware;rdma")
endif()
```

## Unit Test Case Batches

The unit-test implementation should proceed in small reviewable batches. Each
batch should build and pass before the next one starts.

### Batch 1 -- Pure Backend-Neutral Unit Tests

These tests should not require RDMA hardware and should use the Asio-style
`tests/unit/unit_test.hpp` harness.

`tests/unit/rdma/error.cpp`:

- `rdma_errc` maps to `rdma_error_category`.
- Category name is stable.
- Representative messages are backend-neutral.
- `rdma_errc` has `std::is_error_code_enum` support.
- `asio::error_code ec = rdma_errc::...` implicit conversion works.
- ADL `make_error_code(rdma_errc::...)` works.
- `ec == rdma_errc::...` and `ec != rdma_errc::...` comparisons work.
- Default `asio::error_code{}` is success; `rdma_errc` should not grow a
  `success` enumerator.

`tests/unit/rdma/buffer.cpp`:

- `mutable_buffer` default state is `{nullptr, 0, 0}`.
- `const_buffer` default state is `{nullptr, 0, 0}`.
- Mutable-to-const conversion preserves address, length, and lkey.
- Single-buffer ADL `buffer_sequence_begin/end` works for `mutable_buffer`.
- Single-buffer ADL `buffer_sequence_begin/end` works for `const_buffer`.
- `std::vector`, `std::array`, and `std::list` buffer sequences satisfy the
  expected concepts.
- `detail::buffer_size()` returns zero for empty/all-empty sequences.
- `detail::buffer_size()` sums multi-buffer lengths.
- `detail::all_empty()` returns true for all zero-length buffers.
- `detail::all_empty()` returns false for mixed or non-empty buffers.
- Fake-MR `buffer(mr)` returns `mutable_buffer` for non-const MR.
- Fake-MR `buffer(const mr)` returns `const_buffer`.
- Fake-MR valid slices compute `addr + offset`, `length`, and `lkey`.
- Fake-MR invalid slices return an empty buffer.

`tests/unit/rdma/tcp.cpp`:

- `tcp::v4().any_endpoint(port)` has the requested port and IPv4 family.
- `tcp::v6().any_endpoint(port)` has the requested port and IPv6 family.
- Backend-specific `tcp::connector` and `tcp::listener` aliases compile.
- Backend-agnostic aliases from `rdma_types.hpp` compile.

`tests/unit/rdma/public_api_compile.cpp`:

- `#include "rdma/rdma.hpp"` compiles as a standalone umbrella include.
- Public aliases compile:
  `rdma_connector<tcp>`, `rdma_listener<tcp>`, `rdma_queue_pair`,
  `rdma_completion_queue`, `rdma_memory_region`, `rdma_device_ptr`.
- Public completion tokens compile for connector/listener/queue-pair async
  overloads where no hardware execution is needed.

### Batch 2 -- Backend Detail Helper Unit Tests

These tests should still avoid live RDMA hardware whenever possible.

`tests/unit/nd/config_derive.cpp`:

- Port existing `tests/nd/test_nd_config_derive.cpp` to the Asio-style harness.
- Defaults derive from device caps and library limits.
- Defaults respect small device caps.
- Explicit user values are preserved according to the current contract.
- Zero or very small caps are covered explicitly.
- `max_inline_data_`, inbound read limit, and outbound read limit defaults are
  covered.
- Backlog remains a public config value and is not accidentally coupled to
  device caps.

`tests/unit/nd/error.cpp`:

- Native ND category name is stable.
- Representative native messages are stable: `ND_SUCCESS`, `ND_CANCELED`, and
  one failure code such as `ND_INVALID_PARAMETER`.
- `ND_PENDING` is not a public `nd_errc` enumerator.
- Manual `make_nd_error_code(ND_PENDING)` is treated as an internal diagnostic
  and does not become a public success/error branch.
- User-visible cancellation paths are expected to use
  `asio::error::operation_aborted`, not `nd_errc::canceled`.

`tests/unit/nd/buffer.cpp`:

- `buffers2sglist` converts a single RDMA buffer to one `ND2_SGE`.
- Multi-buffer sequences preserve order.
- `std::list` / forward-iterator sequences are supported.
- Empty sequences leave the SGE list empty.
- SGE fields are exact: `Buffer`, `BufferLength`, `MemoryRegionToken`.

`tests/unit/ibv/config_derive.cpp`:

- Add the IBV counterpart to ND config derivation tests.
- Defaults derive from verbs device caps and library limits.
- Small caps, explicit values, inline data, read limits, and CM timeout are
  covered.

`tests/unit/ibv/error_helpers.cpp`:

- `make_system_error_code(errno_value)` uses `std::system_category()`.
- `last_system_error()` captures the current `errno`.
- Messages are printable.
- There is no public `ibv_errc` enum dependency.

`tests/unit/ibv/buffer.cpp`:

- `buffers2sglist` converts a single RDMA buffer to one `ibv_sge`.
- Multi-buffer sequences preserve order.
- `std::list` / forward-iterator sequences are supported.
- Empty sequences leave the SGE list empty.
- SGE fields are exact: `addr`, `length`, `lkey`.

### Batch 3 -- Public API Guard Tests

These tests exercise public API error contracts. Some can run without hardware;
hardware-dependent parts should be isolated and labelled.

Device manager:

- `instance()` returns the same singleton address.
- Impossible config returns no device.
- If hardware exists, discovered device has a non-null native handle and a
  non-empty name.

`use_device`:

- Null device returns `rdma_errc::invalid_device`.
- Duplicate registration returns `rdma_errc::already_registered`.
- Successful registration exposes an effective config.
- Successful registration initializes the backend completion service.

Connector/listener:

- No `use_device` returns `rdma_errc::device_not_registered`.
- Duplicate `open()` returns `asio::error::already_open`.
- Invalid adopted handle returns `rdma_errc::invalid_handle`.
- `listener::bind(port)` is the only public bind shape; address binding remains
  backend implementation detail.

Queue pair:

- Default state is unbound.
- Default `bound_type()` is `completion_mode::none`.
- Event-mode bind sets `bound_type()` to `completion_mode::event`.
- Poll-mode bind sets `bound_type()` to `completion_mode::poll`.
- Duplicate bind/open guards are exact.
- Async send/recv/read/write overloads compile for the public buffer sequence
  types.

Memory region:

- Invalid device construction fails with `rdma_errc::invalid_device`.
- `addr()`, `length()`, `local_key()`, and `remote_addr()` are stable after
  successful registration.
- `is_in_mr(offset, length)` covers start, middle, end, zero-length, and
  out-of-range cases.
- `slice()` and `cslice()` return valid RDMA buffers for valid ranges.
- Invalid slices return empty buffers or the documented error behavior.
- Moved-from or unregistered MR access returns `rdma_errc::invalid_handle` where
  applicable.

### Batch 4 -- Header Layout Compile Tests

These tests protect the planned move to `include/rdma/nd` and
`include/rdma/ibv`.

- `#include "rdma/rdma.hpp"` compiles.
- On ND builds, these compile:
  - `#include "rdma/nd/nd_error.hpp"`
  - `#include "rdma/nd/nd_connector.hpp"`
  - `#include "rdma/nd/nd_listener.hpp"`
  - `#include "rdma/nd/nd_queue_pair.hpp"`
  - `#include "rdma/nd/nd_mr.hpp"`
- On IBV builds, these compile:
  - `#include "rdma/ibv/ibv_error.hpp"`
  - `#include "rdma/ibv/ibv_connector.hpp"`
  - `#include "rdma/ibv/ibv_listener.hpp"`
  - `#include "rdma/ibv/ibv_queue_pair.hpp"`
  - `#include "rdma/ibv/ibv_mr.hpp"`
- After migration, new unit tests and examples must not include `"nd/..."`
  or `"ibv/..."`.

### Batch 5 -- Hardware Integration Registration

These are existing behavior tests that should be registered with CTest labels
once the unit-test foundation is in place.

- `rdma/private_data.cpp`: private-data request/reply matrix, outgoing lifetime,
  no-reply overloads, and cancellation-slot propagation.
- `rdma/sgl.cpp`: multi-MR gather/scatter and too-many-SGE rejection.
- `rdma/echo.cpp`: event-mode echo.
- `rdma/echo_poll.cpp`: poll-mode echo.
- `nd/control_cancel.cpp` and `ibv/control_cancel.cpp`: per-operation
  control-plane cancellation.
- `nd/disconnect_cancel.cpp` and `ibv/disconnect_cancel.cpp`: cross-thread
  disconnect races and connector terminal state.
- `nd/wait_disconnect.cpp` and `ibv/wait_disconnect.cpp`: graceful disconnect
  watcher and level-trigger behavior.

These tests should be labelled `hardware` and `integration`; cancellation and
race-oriented tests should also be labelled `race`.

Status:

- Single-process hardware tests are registered behind
  `RDMA_ENABLE_HARDWARE_TESTS`.
- Address-dependent tests are additionally gated by `RDMA_TEST_ADDR`.
- Two-process echo/example executables remain manual until a dedicated launcher
  or fixture harness exists.

### Batch 6 -- Later Hardware Regression Tests

- RDMA read/write round trip.
- Zero-length send/recv behavior.
- Multi-message ordering.
- Negative connect behavior: unreachable address, no listener, invalid port
  family.

Stress, performance, latency, multi-QP throughput, and soak tests are moved to
`docs/rdma_stress_performance_plan.md`.

Status:

- `test_rdma_regression` covers read/write round trip, zero-length behavior,
  multi-message ordering, and no-listener negative connect.
- Invalid port-family and broader unreachable-address matrices can be added as
  future targeted regressions if a bug or backend discrepancy appears.

## Unit Coverage Matrix

### Backend-Neutral Public API

Target files:

- `include/rdma/rdma.hpp`
- `include/rdma/rdma_types.hpp`
- `include/rdma/rdma_commons.hpp`
- `include/rdma/rdma_error.hpp`
- `include/rdma/rdma_buffer.hpp`
- `include/rdma/tcp.hpp`

Tests:

- `rdma_errc` category name, messages, comparison, ADL `make_error_code`,
  implicit `asio::error_code` conversion.
- `rdma_config_t` default values.
- `rdma_remote_addr_t` value semantics.
- `completion_mode` default state expectations through queue-pair public API.
- `tcp::v4()`, `tcp::v6()`, `any_endpoint(port)` address family and port.
- `rdma/rdma.hpp` umbrella include compile test.
- Backend-agnostic aliases compile test:
  `rdma_connector<tcp>`, `rdma_listener<tcp>`, `rdma_queue_pair`,
  `rdma_completion_queue`, `rdma_memory_region`, `rdma_device_ptr`.

### Backend-Neutral Buffer Helpers

Target files:

- `include/rdma/rdma_buffer.hpp`
- `include/rdma/detail/rdma_op_send.hpp`
- `include/rdma/detail/rdma_op_recv.hpp`
- `include/rdma/detail/rdma_op_read.hpp`
- `include/rdma/detail/rdma_op_write.hpp`

Tests:

- `mutable_buffer` and `const_buffer` default state.
- Mutable-to-const implicit conversion preserves address, length, and lkey.
- Single-buffer ADL `buffer_sequence_begin/end`.
- Vector, array, initializer-list, and `std::list` buffer sequences satisfy the
  relevant concepts.
- `detail::buffer_size()` sums lengths.
- `detail::all_empty()` handles all-empty, mixed, and zero-length sequences.
- `buffer(mr)` and `buffer(mr, offset, length)` using a fake MR:
  - non-const MR returns `mutable_buffer`;
  - const MR returns `const_buffer`;
  - valid slices compute address + offset;
  - invalid slices return an empty buffer.
- Operation objects compute byte counts correctly for empty and multi-buffer
  sequences where construction can be tested without posting to hardware.

### ND Detail Unit Tests

Target files:

- `include/rdma/nd/detail/nd_config_derive.hpp`
- `include/rdma/nd/nd_buffer.hpp`
- `include/rdma/nd/nd_error.hpp`
- `include/rdma/nd/detail/nd_ops_cm.hpp`
- `include/rdma/nd/detail/nd_ops_verbs.hpp`
- `include/rdma/nd/detail/nd_service_*`

Tests:

- Expand `derive_effective_config`:
  - defaults clamp to device caps;
  - explicit values are preserved or clamped according to the chosen contract;
  - zero device caps and very small caps;
  - read-limit defaults;
  - inline data defaults;
  - backlog remains a public config value.
- `nd_errc` native category:
  - representative native messages;
  - `ND_PENDING` is not a public enum value and maps to unknown if manually
    converted via `make_nd_error_code`;
  - `ND_CANCELED` is still native, while user-visible cancellation paths map to
    `asio::error::operation_aborted`.
- `buffers2sglist`:
  - single buffer;
  - multiple buffers;
  - forward-only container such as `std::list`;
  - empty sequence leaves SGE list empty;
  - SGE fields use `Buffer`, `BufferLength`, `MemoryRegionToken`.
- Service guard behavior that does not need real hardware:
  - opening connector/listener/queue_pair before `use_device` returns
    `rdma_errc::device_not_registered`;
  - invalid adopted connector/listener handles return `rdma_errc::invalid_handle`
    where a public assign path exists;
  - duplicate object `open()` returns `asio::error::already_open`.
- Internal `ND_PENDING` policy:
  - wrappers that initiate overlapped ops should leave `ec` clear for
    `ND_SUCCESS` and `ND_PENDING`;
  - callers should branch on raw `HRESULT`, not `nd_errc::pending`.
  - If direct native mocking is hard, cover this through small fake wrapper tests
    or refactor the success/pending conversion into a tiny testable helper.

### IBV Detail Unit Tests

Target files:

- `include/rdma/ibv/detail/ibv_config_derive.hpp`
- `include/rdma/ibv/ibv_buffer.hpp`
- `include/rdma/ibv/ibv_error.hpp`
- `include/rdma/ibv/detail/ibv_ops_cm.hpp`
- `include/rdma/ibv/detail/ibv_ops_verbs.hpp`
- `include/rdma/ibv/detail/ibv_service_*`

Tests:

- Add an IBV counterpart to `test_nd_config_derive`.
- `make_system_error_code(errno)` and `last_system_error()`:
  - category is `std::system_category()`;
  - message is printable;
  - no `ibv_errc` public enum remains.
- `buffers2sglist`:
  - single and multiple buffers;
  - forward-only container;
  - empty sequence;
  - SGE fields use `addr`, `length`, `lkey`.
- Service guard behavior:
  - duplicate `open()` returns `asio::error::already_open`;
  - no `use_device` returns `rdma_errc::device_not_registered`;
  - invalid adopted handle returns `rdma_errc::invalid_handle`.
- Connector terminal guard:
  - `async_connect` and `async_accept` both reject non-idle connectors with
    `rdma_errc::connector_terminal`.
  - Prefer fake/service-level unit tests if possible; keep full RDMA race tests
    in integration.

### Public Backend APIs

Target files:

- `include/rdma/nd/*.hpp`
- `include/rdma/ibv/*.hpp`
- `include/rdma/*.hpp`

Tests:

- `device_manager`:
  - singleton identity;
  - impossible config returns no device;
  - found device has non-null native context and name when hardware exists.
- `use_device`:
  - null device -> `rdma_errc::invalid_device`;
  - duplicate registration -> `rdma_errc::already_registered`;
  - registered service exposes effective config and shared CQ.
- `completion_queue`:
  - default construction;
  - bind/open state;
  - `poll_one` / `poll` behavior with no completions where applicable.
- `queue_pair`:
  - default state `is_bound() == false`, `bound_type() == none`;
  - event bind and poll bind state;
  - duplicate bind/open behavior;
  - async send/recv/read/write compile and empty-buffer completion behavior.
- `memory_region`:
  - constructor rejects invalid device;
  - `addr`, `length`, `local_key`, `remote_key`, `remote_addr`;
  - `is_in_mr` boundary cases;
  - `slice` / `cslice` valid and invalid ranges;
  - moved-from behavior returns `rdma_errc::invalid_handle` where applicable.
- `connector`:
  - default state;
  - `open`, duplicate `open`, `assign`, `is_open`;
  - `async_connect` overload matrix: with request/reply, no reply, no request;
  - terminal reuse returns `rdma_errc::connector_terminal`;
  - `disconnect` idempotence;
  - `async_wait_disconnect` level-trigger behavior.
- `listener`:
  - default state;
  - `open`, duplicate `open`, `bind(port)`, `listen`;
  - `async_get_connection` overload matrix;
  - `cancel` aborts pending get and leaves listener reusable.

## Integration Coverage Matrix

Keep and improve current integration tests:

- `rdma_echo`: event-mode send/recv.
- `rdma_echo_poll`: poll-mode send/recv.
- `rdma_private_data`: request/reply matrix, outgoing lifetime, no-reply
  overloads, cancellation-slot propagation.
- `rdma_sgl`: multi-MR gather/scatter and too-many-SGE rejection.
- `*_wait_disconnect`: peer graceful disconnect and level-trigger re-arm.
- `*_control_cancel`: per-operation cancellation for control-plane ops.
- `*_disconnect_cancel`: cross-thread disconnect races and terminal connector.

Add later:

- RDMA read/write round-trip test.
- Multi-message ordering test.
- Zero-length send/recv behavior.
- Negative connect tests: unreachable address, no listener, invalid port family.

Concurrency stress and performance-oriented multi-QP coverage is tracked in
`docs/rdma_stress_performance_plan.md`.

## Test Style Guidelines

- Use `ASIO_CHECK` for independent assertions. When later checks depend on a
  condition, use a normal early return after an `ASIO_CHECK_MESSAGE` failure.
- Prefer exact error-code comparisons over `assert(ec)`.
- Every hardware test should print the backend, address, and port range.
- Avoid sleeps where possible; use timers as watchdogs and fail explicitly on
  timeout.
- Keep unit tests deterministic and under one second.
- Keep integration tests parameterized by CMake cache values or command-line
  args.
- Avoid non-ASCII in source comments and test output.

## Additional Test Policies

### Keep Test Source Build-System Neutral

Unit test source should not depend on CTest, CMake, or CMake cache variables
directly. The same `.cpp` should be usable later from Asio's own build system.

Allowed in test source:

- Asio-style `unit_test.hpp` macros.
- Normal command-line arguments for integration tests.
- Preprocessor checks for platform/backend selection when unavoidable.

Avoid in test source:

- CTest-specific behavior.
- CMake-generated headers for ordinary unit-test logic.
- Assumptions about executable output paths.

CMake/CTest should only compile, label, parameterize, and run test executables.
The test executable's exit code remains the contract.

### Skip And Hardware Policy

Pure unit tests must not skip due to missing RDMA hardware. If a test can skip
because no RDMA device or address is available, it is not a pure unit test and
should be labelled `hardware` or `integration`.

Hardware tests may skip only for environmental preconditions:

- no RDMA device;
- no configured `RDMA_TEST_ADDR`;
- backend unavailable on the current platform.

If a hardware test starts an operation and then observes an unexpected error,
that is a failure, not a skip.

### Fake And Mock Boundary

Prefer small value fakes for pure helpers:

- fake memory-region objects for `rdma_buffer` tests;
- fake native capability structs for config derivation;
- fake buffer sequences for SGE conversion.

Avoid heavy mocking of native RDMA APIs (`IND2*`, `ibv_*`, `rdma_cm`) unless the
production code has already been factored behind a tiny stable adapter. Native
provider behavior should generally be covered by integration tests rather than
fragile mocks.

If a helper is hard to test without native calls, consider extracting a tiny pure
function first. Example: converting native initiation status into
`{HRESULT, asio::error_code}` policy can be unit-tested without mocking an
`IND2Connector`.

### Failure Diagnostics

When a check can fail in a non-obvious way, use `ASIO_CHECK_MESSAGE` and include
the actual value:

- error category name;
- integer error value;
- `error_code.message()` for diagnostics only;
- buffer address, length, and lkey;
- queue-pair bound mode.

Do not assert localized or platform-specific `std::system_category()` message
strings. For system errors, compare category and value; print message only for
diagnostics.

### Regression Test Rule

Every bug fix should either:

- add a pure unit test that fails before the fix; or
- add/extend an integration regression when the behavior requires real RDMA
  hardware.

If a bug is too timing-sensitive for a deterministic test, document the reason
in the test or plan and add a smaller deterministic guard around the relevant
state transition.

### CI And Local Test Gates

Minimum local gate for ordinary changes:

```text
cmake --build build --config Debug
ctest --test-dir build -C Debug -L unit --output-on-failure
```

For public API or header layout changes:

```text
ctest --test-dir build -C Debug -L "unit|compile" --output-on-failure
```

For backend behavior changes on a machine with RDMA hardware:

```text
ctest --test-dir build -C Debug -L hardware --output-on-failure
```

Planned CI matrix:

- Windows + ND: unit/compile always; hardware tests optional on an RDMA runner.
- Linux + IBV: unit/compile always; hardware tests optional on an RDMA runner.
- No-hardware runners must still pass the default unit/compile suite.

## Implementation Stages

### Stage 1 -- Test Infrastructure

- Add an Asio-style `tests/unit/unit_test.hpp` harness, copied/adapted from
  Asio's `src/tests/unit/unit_test.hpp` with the original license notice.
- Add `tests/unit/CMakeLists.txt`.
- Add helper CMake function:
  `rdma_add_unit_test(name source labels...)`.
- Add labels to current CTest registrations.
- Register only hardware-independent tests by default.

### Stage 2 -- Backend-Neutral Unit Tests

- Convert `test_rdma_error` to the Asio-style harness or add an equivalent
  `unit/rdma/test_error.cpp`.
- Add `unit/rdma/test_buffer.cpp`.
- Add `unit/rdma/test_tcp.cpp`.
- Add `unit/rdma/test_public_api_compile.cpp`.

### Stage 3 -- Detail Helper Tests

- Move/convert `test_nd_config_derive`.
- Add `unit/ibv/test_config_derive.cpp`.
- Add ND/IBV `buffers2sglist` tests.
- Add error-helper tests for ND and IBV.

### Stage 4 -- Public API Guard Tests

- Strengthen existing connector/listener/use_device tests:
  exact error codes, duplicate opens, moved-from/invalid-handle behavior.
- Add memory-region boundary tests.
- Add queue-pair bind-mode tests.

### Stage 5 -- Integration Test Registration

- Register existing hardware tests with `hardware` labels behind
  `RDMA_ENABLE_HARDWARE_TESTS`.
- Add CMake cache args for IP/port.
- Split long/race tests into `race` label so fast hardware smoke can run
  separately from longer stress runs owned by
  `docs/rdma_stress_performance_plan.md`.

Status: completed for single-process hardware tests and existing race tests.
Two-process example-style echo programs remain manual executables.

### Stage 6 -- Hardware Regression Suite

- Add read/write integration tests.
- Add zero-length and multi-message ordering integration tests.
- Add negative connect integration tests.
- Keep soak, stress, throughput, and latency programs in
  `docs/rdma_stress_performance_plan.md`.

Status: completed in `tests/rdma/test_rdma_regression.cpp` for the first
read/write, zero-length, ordering, and no-listener negative-connect matrix.

## Acceptance Criteria

- `ctest -L unit` passes without RDMA hardware.
- `ctest` default passes on a machine without RDMA hardware.
- With `RDMA_ENABLE_HARDWARE_TESTS=ON` and a valid local RDMA IP, all current
  hardware tests are registered and runnable through CTest.
- Public API coverage exists for every externally documented class/function.
- Critical `detail` helpers have deterministic tests that fail with clear
  expected/actual diagnostics.
- ND and IBV backend parity is visible in the test matrix.
- Stress/performance/latency acceptance criteria are owned by
  `docs/rdma_stress_performance_plan.md`.
