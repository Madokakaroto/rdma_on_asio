cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project(rdma_on_asio VERSION 0.1.0 LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RDMA_BUILD_UNIT_TESTS "Build deterministic unit tests" ON)
option(RDMA_BUILD_INTEGRATION_TESTS "Build smoke and integration tests" ON)
option(RDMA_BUILD_STRESS_TESTS "Build opt-in RDMA stress tests" OFF)
option(RDMA_BUILD_PERFORMANCE_TESTS "Build opt-in RDMA performance and latency tools" OFF)
option(RDMA_BUILD_NATIVE_BASELINES "Build opt-in native backend baseline tools" OFF)
option(RDMA_BUILD_ASIO_MACRO_TESTS "Build opt-in ASIO macro mode validation tests" OFF)
option(RDMA_ENABLE_PERFTEST_BASELINE "Enable optional perftest baseline helpers" OFF)
option(RDMA_BUILD_PERFTEST "Build/import a project-managed perftest checkout for benchmark use" OFF)
option(RDMA_ENABLE_HARDWARE_TESTS "Register tests that need RDMA hardware" OFF)
set(RDMA_TEST_ADDR "" CACHE STRING "Local RDMA-capable IP address for hardware tests")
set(RDMA_TEST_PEER_ADDR "" CACHE STRING "Peer RDMA-capable IP address for two-host hardware tests")
set(RDMA_TEST_BASE_PORT "5000" CACHE STRING "Base TCP/RDMA port for hardware tests")
set(RDMA_PERFTEST_MODE "system" CACHE STRING "perftest mode: system, external, or managed")
set(RDMA_PERFTEST_BIN_DIR "" CACHE STRING "Directory containing prebuilt perftest executables")

# RDMA backend detection
if(WIN32)
  set(RDMA_BACKEND "nd" CACHE STRING "RDMA backend (nd, ibv)")
else()
  set(RDMA_BACKEND "ibv" CACHE STRING "RDMA backend (nd, ibv)")
endif()

# NetworkDirect backend
if(RDMA_BACKEND STREQUAL "nd")
  include(cmake/NetworkDirect.cmake)
  rdma_networkdirect_init(
      OUT_INCLUDE_DIRS NETWORKDIRECT_INCLUDE_DIRS
      OUT_LIB_DIR NETWORKDIRECT_LIB_DIR
      OUT_LIB_NAME NETWORKDIRECT_LIB_NAME)
endif()

# libibverbs backend (Linux rdma-core: libibverbs + librdmacm)
if(RDMA_BACKEND STREQUAL "ibv")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(IBVERBS REQUIRED IMPORTED_TARGET libibverbs)
  pkg_check_modules(RDMACM  REQUIRED IMPORTED_TARGET librdmacm)
  message(STATUS "RDMA backend: ibv (libibverbs ${IBVERBS_VERSION}, librdmacm ${RDMACM_VERSION})")
endif()

# Unit tests
if(RDMA_BUILD_UNIT_TESTS)
  add_subdirectory(tests/unit)
endif()

# Smoke/integration tests
if(RDMA_BUILD_INTEGRATION_TESTS)
  if(RDMA_BACKEND STREQUAL "nd")
    add_subdirectory(tests/nd)
  elseif(RDMA_BACKEND STREQUAL "ibv")
    add_subdirectory(tests/ibv)
  endif()

  # Cross-platform tests (backend-agnostic rdma_* API), built for either backend.
  add_subdirectory(tests/rdma)
endif()

if(RDMA_BUILD_STRESS_TESTS OR RDMA_BUILD_PERFORMANCE_TESTS OR RDMA_BUILD_NATIVE_BASELINES)
  add_subdirectory(tests/benchmark)
endif()
