# CMake Test Unification Plan

## Principles

1. `BUILD_TESTING` is the only switch for adding the `tests/` build graph.
2. `tests/CMakeLists.txt` owns every executable under `tests/`.
3. Nested test directories are source layout only; they do not call
   `add_executable()` independently.
4. CTest only runs already-built executables. It does not decide what gets
   built.
5. Automated CTest coverage is local-machine oriented. Multi-host runs remain
   manual/runtime tooling for now.
6. CMake scripts must not contain local or peer RDMA address cache variables.
   Local RDMA addresses are discovered by test code at runtime.

## Root CMake

Use standard CMake/CTest behavior:

```cmake
include(CTest)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
```

`include(CTest)` defines the standard `BUILD_TESTING` cache option. Remove
project-specific duplicates such as `RDMA_BUILD_*_TESTS`.

When `BUILD_TESTING=OFF`, no test executable targets are declared and no CTest
tests are registered.

## Tests CMake

`tests/CMakeLists.txt` becomes the only orchestration point for test
executables:

```text
tests/CMakeLists.txt
  |
  |-- unit / compile / API-shape tests
  |-- backend tests for the active backend
  |-- backend-agnostic rdma_* tests
  |-- ASIO macro tests
  |-- performance benchmarks
  |-- stress tests
  |-- native/perftest baselines
  +-- CTest registrations
```

Nested directories remain source layout only:

```text
tests/unit/       source files
tests/nd/         source files
tests/ibv/        source files
tests/rdma/       source files
tests/benchmark/  source files, scripts, scenarios
tests/stress/     source files
```

Existing nested `CMakeLists.txt` files should be removed, emptied, or converted
to comments/source-list fragments. They should not independently declare
targets.

## Build Policy

With `BUILD_TESTING=ON`, build all executables used by test workflows:

- unit and compile tests;
- backend integration binaries;
- backend-agnostic RDMA API binaries;
- ASIO macro validation binaries;
- performance benchmark binaries;
- stress/soak binaries;
- baseline binaries.

Do not gate executable construction on hardware availability or runtime address
configuration. The project is expected to run in an RDMA-capable environment.

## Runtime Address Discovery

Single-host RDMA tests should discover a usable local RDMA-capable address in
the executable itself. Test code should call the public RDMA-on-Asio helper
(`asio::rdma::query_local_rdma_address`) via a small shared test helper; it must
not inspect NetworkDirect/ibverbs objects or call raw backend APIs directly.

CTest should register local-machine address-using tests unconditionally:

```text
CTest command:
  omits local address arguments

Executable/test helper:
  queries a usable local RDMA address at runtime
```

The affected test/benchmark entry points must not take a local address for
automated single-host runs:

- smoke tests accept at most `[port]`;
- benchmark tools using `--single-process` auto-discover the local address;
- stress tools auto-discover the local address;
- native ND baseline tools auto-discover the local address for single-process
  baseline runs.

Peer addresses are intentionally not CMake cache inputs. Multi-host topology is
a runtime concern, and binding a peer address at configure time would make the
build tree environment-specific. Users who run two-host programs should provide
the peer address through the executable, script, or a future multi-host CTest
orchestration layer.

Current multi-host status:

- Some executables already have separate server/client modes and can be run
  manually across machines.
- Benchmark tooling already models two-host command lines through `--server`,
  `--client`, `--peer-addr`, and topology names such as `two_host_direct`.
- This refactor does not add automated multi-host orchestration.

## CTest Policy

Keep the first CTest layer intentionally small:

```text
unit
performance
stress
baseline
```

Initial grouping:

- `unit`: deterministic unit, compile, API-shape, backend smoke, and ASIO macro
  validation tests.
- `performance`: asio performance tools and performance smoke tests.
- `stress`: stress/soak tests.
- `baseline`: native/perftest baseline tests.

Plain `ctest` may run all registered tests for the current configuration.
Presets and labels are convenience filters, not build gates.

Example commands:

```powershell
ctest --test-dir build -C Debug -L unit --output-on-failure
ctest --test-dir build -C Release -L performance --output-on-failure
ctest --test-dir build -C Release -L stress --output-on-failure
ctest --test-dir build -C Release -L baseline --output-on-failure
```

## CTest Presets

Add CTest presets in the same refactor, using only the four initial names:

```json
{
  "testPresets": [
    { "name": "unit", "filter": { "include": { "label": "unit" } } },
    { "name": "performance", "filter": { "include": { "label": "performance" } } },
    { "name": "stress", "filter": { "include": { "label": "stress" } } },
    { "name": "baseline", "filter": { "include": { "label": "baseline" } } }
  ]
}
```

Presets should not imply a build. Users still need to build the matching
configuration first:

```powershell
cmake --build build --config Release
ctest --preset performance
```

## Options

Remove build-style suite switches:

```cmake
RDMA_BUILD_UNIT_TESTS
RDMA_BUILD_INTEGRATION_TESTS
RDMA_BUILD_STRESS_TESTS
RDMA_BUILD_PERFORMANCE_TESTS
RDMA_BUILD_NATIVE_BASELINES
RDMA_BUILD_ASIO_MACRO_TESTS
RDMA_ENABLE_HARDWARE_TESTS
RDMA_ENABLE_PERFTEST_BASELINE
```

Use only standard `BUILD_TESTING` for whether tests are added.

Keep non-address configuration/capability inputs:

```cmake
RDMA_TEST_BASE_PORT
RDMA_PERFTEST_MODE
RDMA_PERFTEST_BIN_DIR
RDMA_BUILD_PERFTEST
```

No RDMA address should appear in CMake scripts. Local addresses are discovered by
test code; peer addresses are supplied manually to multi-host runtime tooling
outside this first-pass CTest graph.

## Migration Steps

1. Root CMake: replace per-suite test options with `include(CTest)` and
   `if(BUILD_TESTING) add_subdirectory(tests) endif()`.
2. Move all test executable declarations into `tests/CMakeLists.txt`.
3. Remove nested test `add_subdirectory()` orchestration.
4. Convert nested `CMakeLists.txt` files to source-list fragments or comments.
5. Add or reuse a shared runtime helper that queries the active backend for a
   usable local RDMA address.
6. Update single-host smoke/performance/stress/baseline entry points so CTest
   can omit local address arguments entirely.
7. Ensure `BUILD_TESTING=ON` builds unit, performance, stress, baseline, ASIO
   macro, and RDMA-address-dependent binaries by default.
8. Register tests with one of the initial labels: `unit`, `performance`,
   `stress`, `baseline`.
9. Add CTest presets for `unit`, `performance`, `stress`, and `baseline`.
10. Update README/docs commands.

## Verification

- `BUILD_TESTING=OFF` configures without adding `tests/`.
- `BUILD_TESTING=ON` builds all test-related executables.
- No CMake script references `RDMA_TEST_ADDR`, `RDMA_TEST_PEER_ADDR`, or any
  other local/peer RDMA address cache variable.
- Single-host address-using tests run by querying a local RDMA-capable address
  at runtime through the public RDMA-on-Asio API.
- Address-using test code contains no direct NetworkDirect or ibverbs API calls;
  native baseline code is the only intentional raw-backend comparison path.
- Plain `ctest` runs the registered tests for the current configuration.
- `ctest -L unit` selects unit-labeled tests.
- `ctest -L performance` selects performance-labeled tests.
- `ctest -L stress` selects stress-labeled tests.
- `ctest -L baseline` selects baseline-labeled tests.
- `ctest --preset unit` works after the matching configuration has been built.
- `ctest --preset performance` works after the matching configuration has been
  built.
- `ctest --preset stress` works after the matching configuration has been built.
- `ctest --preset baseline` works after the matching configuration has been
  built.
- ND and ibv backend configurations only build backend-valid targets.
