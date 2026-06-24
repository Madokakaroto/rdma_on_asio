# ND CMake Refactor Plan

## Status

已落地。

本计划合并并取代以下历史文档：

- `docs/networkdirect_compilation_mode_plan.md`
- `docs/nd_asio_macro_plan.md`
- `docs/networkdirect_cmake_refactor_plan.md`
- `docs/windows_executable_cmake_refactor_plan.md`

旧文档中的 embedded-only、native-ndutil opt-in、CMake 推导 Asio 宏、CMake 定义
`ASIO_SEPARATE_COMPILATION` / `ASIO_NO_DEFAULT_LINKED_LIBS` 等内容均已废弃。

## Final Model

ND backend 的 CMake 分成两层：

```text
NetworkDirect.cmake
  |
  |-- mc.exe -> generated/networkdirect/ndstatus.h
  |
  +-- MSBuild.exe third_party/networkdirect/src/ndutil/ndutil.vcxproj
        |
        v
      networkdirect-native/<config>-<platform>/ndutil/ndutil.lib

Windows executable targets
  |
  |-- consume NetworkDirect include path
  |-- consume NetworkDirect lib path
  |-- explicitly choose source wrapper TU list
  +-- explicitly choose manual link libraries when needed
```

`NetworkDirect.cmake` 只负责准备 NetworkDirect native 产物和返回路径；每个 Windows
executable 自己声明 source list 和 link libraries。

## NetworkDirect Module

模块文件：

```text
cmake/NetworkDirect.cmake
```

公开入口：

```cmake
init_networdirect()
```

Exported variables:

```cmake
NETWORKDIRECT_SOURCE
  src/networkdirect.cpp

NETWORKDIRECT_LIB_NAME
  ndutil.lib
```

内部 target：

```text
rdma_networkdirect_generated_headers
  produces generated/networkdirect/ndstatus.h

rdma_networkdirect_headers
  interface include surface

rdma_networkdirect_native_ndutil
  depends on generated headers
  produces networkdirect-native/<config>-<platform>/ndutil/ndutil.lib

rdma_networkdirect_backend
  interface target applied by init_networdirect()
  provides _WIN32_WINNT, include paths, lib search path, and build-order deps
```

### Module Rules

- `NetworkDirect.cmake` always builds native `ndutil.lib`.
- `NetworkDirect.cmake` does not inspect `ASIO_SEPARATE_COMPILATION`.
- `NetworkDirect.cmake` does not inspect `ASIO_NO_DEFAULT_LINKED_LIBS`.
- `NetworkDirect.cmake` does not define either Asio macro for consumers.
- `NetworkDirect.cmake` does not decide whether an executable should compile
  `src/networkdirect.cpp`.
- `NetworkDirect.cmake` does not decide whether an executable should manually link
  `ndutil.lib`.
- `rdma_networkdirect_backend` is an interface target, not a normal link library
  for `ndutil.lib`.
- `init_networdirect()` applies `rdma_networkdirect_backend` to targets
  created after initialization, so individual executable targets do not need a
  per-target ND helper call.

## Root CMake Shape

Root `CMakeLists.txt` owns only backend selection and module initialization:

```cmake
if(RDMA_BACKEND STREQUAL "nd")
  include(cmake/NetworkDirect.cmake)
  init_networdirect()
endif()
```

`init_networdirect()` exports `NETWORKDIRECT_SOURCE` for executable source lists,
exports `NETWORKDIRECT_LIB_NAME` for manual-link targets, and applies
`rdma_networkdirect_backend` as a directory-level usage requirement for later
targets.

`rdma_networkdirect_backend` is intentionally thin. It must not:

- add `src/networkdirect.cpp`;
- add `asio/impl/src.hpp`;
- define `ASIO_SEPARATE_COMPILATION`;
- define `ASIO_NO_DEFAULT_LINKED_LIBS`;
- manually link `ndutil.lib`.

## Executable Target Rules

Each Windows executable target declares its own source/link contract locally.

### Embedded NetworkDirect Executables

Targets whose source configuration does not define `ASIO_SEPARATE_COMPILATION` compile
the NetworkDirect wrapper TU:

```cmake
add_executable(my_embedded_test
    my_embedded_test.cpp
    ${NETWORKDIRECT_SOURCE})

target_compile_definitions(my_embedded_test PRIVATE ASIO_STANDALONE)
```

Rules:

- source config does not define `ASIO_SEPARATE_COMPILATION`;
- source config does not define `ASIO_NO_DEFAULT_LINKED_LIBS`;
- target compiles `src/networkdirect.cpp`;
- target does not compile an Asio separate source wrapper;
- target does not manually link `${NETWORKDIRECT_LIB_NAME}`.

### Separate Compilation Autolink Executables

Targets whose source configuration defines `ASIO_SEPARATE_COMPILATION` but not
`ASIO_NO_DEFAULT_LINKED_LIBS` compile an Asio wrapper TU and rely on autolink:

```cmake
add_executable(unit_nd_asio_separate_autolink
    nd/asio_separate_autolink.cpp
    asio_separate_autolink_src.cpp)

target_compile_definitions(unit_nd_asio_separate_autolink PRIVATE ASIO_STANDALONE)
```

Rules:

- source config defines `ASIO_SEPARATE_COMPILATION`;
- source config does not define `ASIO_NO_DEFAULT_LINKED_LIBS`;
- target compiles an Asio separate wrapper TU;
- target does not compile `src/networkdirect.cpp`;
- target does not manually link `${NETWORKDIRECT_LIB_NAME}`;
- `include/rdma/nd/detail/nd_autolink.hpp` emits
  `#pragma comment(lib, "ndutil.lib")` on MSVC.

### Separate Compilation Manual-Link Executables

Targets whose source configuration defines both macros compile an Asio wrapper TU and
manually link NetworkDirect and Windows system libraries:

```cmake
add_executable(unit_nd_asio_separate_manual_link
    nd/asio_separate_manual_link.cpp
    asio_separate_manual_link_src.cpp)

target_compile_definitions(unit_nd_asio_separate_manual_link PRIVATE ASIO_STANDALONE)
target_link_libraries(unit_nd_asio_separate_manual_link PRIVATE
    ${NETWORKDIRECT_LIB_NAME}
    ws2_32
    mswsock
    bcrypt)
```

Rules:

- source config defines `ASIO_SEPARATE_COMPILATION`;
- source config defines `ASIO_NO_DEFAULT_LINKED_LIBS`;
- target compiles an Asio separate wrapper TU;
- target does not compile `src/networkdirect.cpp`;
- target manually links `${NETWORKDIRECT_LIB_NAME}`;
- target manually links Asio/Windows system libraries.

## Source Wrapper Files

Embedded NetworkDirect wrapper:

```text
src/networkdirect.cpp
```

```cpp
#define ASIO_RDMA_NETWORKDIRECT_SOURCE_FILE 1
#include "rdma/nd/impl/networkdirect.hpp"
```

Asio separate wrappers for macro tests:

```text
tests/unit/asio_separate_autolink_src.cpp
tests/unit/asio_separate_manual_link_src.cpp
```

Each includes the matching source configuration header first, then `asio/impl/src.hpp`.

Source configuration headers:

```text
tests/unit/nd/asio_separate_autolink_config.hpp
tests/unit/nd/asio_separate_manual_link_config.hpp
```

These headers are the only test-side place where
`ASIO_SEPARATE_COMPILATION` / `ASIO_NO_DEFAULT_LINKED_LIBS` are defined.

## Autolink

`include/rdma/nd/detail/nd_autolink.hpp` is included by `nd_types.hpp`.

It emits:

```cpp
#pragma comment(lib, "ndutil.lib")
```

only when all of these are true:

```text
ASIO_SEPARATE_COMPILATION
!ASIO_NO_DEFAULT_LINKED_LIBS
_MSC_VER
```

RDMA/ND code does not duplicate Asio's default socket-library pragma behavior.
When `ASIO_NO_DEFAULT_LINKED_LIBS` is defined, the executable target is responsible for
manual system library links.

## Current Implementation Touch Points

- `cmake/NetworkDirect.cmake`
- root `CMakeLists.txt`
- `include/rdma/nd/detail/nd_autolink.hpp`
- `include/rdma/nd/impl/networkdirect.hpp`
- `include/rdma/nd/nd_types.hpp`
- `src/networkdirect.cpp`
- `tests/CMakeLists.txt` (the unified test graph; the former per-subdir
  `tests/{unit,nd,rdma,benchmark}/CMakeLists.txt` are now source-layout-only stubs
  after the CTest unification -- see `cmake_test_unification_plan.md`)
- macro verification test sources under `tests/unit/nd`

## Verification

Completed after implementation:

- Default clean configure/build: passed.
- Default CTest: passed.

> NOTE: the original per-option build/CTest matrix here predates the CTest
> unification (`cmake_test_unification_plan.md` / `ctest_all_tests_plan.md`), which
> **removed** the per-suite build options (`RDMA_BUILD_ASIO_MACRO_TESTS`,
> `RDMA_BUILD_PERFORMANCE_TESTS`, `RDMA_BUILD_NATIVE_BASELINES`, ...). Tests now build
> under the standard `BUILD_TESTING` switch and are selected via CTest labels
> (unit / performance / stress / baseline); the macro and native-baseline variants are
> covered there. The old fixed counts (16/16, 18/18) are not reproducible against the
> current unified graph.

Additional checks:

- `generated/networkdirect/ndstatus.h` is generated by `mc.exe`.
- `networkdirect-native/<config>-<platform>/ndutil/ndutil.lib` is produced by native
  `ndutil.vcxproj`.
- Autolink test target does not explicitly list `ndutil.lib` in linker dependencies.
- Manual-link test target explicitly lists `ndutil.lib`, `ws2_32`, `mswsock`, and
  `bcrypt`.
- Separate macro test targets include the Asio wrapper TU and do not include
  `src/networkdirect.cpp`.
- Embedded targets include `src/networkdirect.cpp` locally in their executable source
  lists.

## Maintenance Rules

- Do not reintroduce CMake logic that branches on `ASIO_SEPARATE_COMPILATION` or
  `ASIO_NO_DEFAULT_LINKED_LIBS`.
- Do not define those two macros with `target_compile_definitions`; use source
  configuration headers.
- Do not make `rdma_networkdirect_backend` add implementation source files or
  directly link `ndutil.lib`.
- Do not make `NetworkDirect.cmake` choose embedded vs separate behavior.
- If a new Windows executable is added, it must explicitly choose:
  - whether to compile `src/networkdirect.cpp`;
  - whether to compile an Asio separate wrapper TU;
  - whether to manually link `${NETWORKDIRECT_LIB_NAME}`.

## Done Criteria

- Old fragmented plans are removed.
- The current ND CMake behavior is documented in this single file.
- Root CMake no longer contains raw `mc.exe`, `ndutil.vcxproj`, or ND target-helper
  logic.
- `NetworkDirect.cmake` applies include path and lib path through
  `rdma_networkdirect_backend`.
- `NetworkDirect.cmake` exports `NETWORKDIRECT_SOURCE` and `NETWORKDIRECT_LIB_NAME`.
- Executable CMakeLists make source/link choices locally.
- ibv backend remains untouched.
