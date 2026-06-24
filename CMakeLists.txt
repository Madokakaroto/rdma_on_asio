cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project(rdma_on_asio VERSION 0.1.0 LANGUAGES CXX)
include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RDMA_BUILD_PERFTEST "Build/import a project-managed perftest checkout for benchmark use" OFF)
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
  init_networdirect()
endif()

# libibverbs backend (Linux rdma-core: libibverbs + librdmacm)
if(RDMA_BACKEND STREQUAL "ibv")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(IBVERBS REQUIRED IMPORTED_TARGET libibverbs)
  pkg_check_modules(RDMACM  REQUIRED IMPORTED_TARGET librdmacm)
  message(STATUS "RDMA backend: ibv (libibverbs ${IBVERBS_VERSION}, librdmacm ${RDMACM_VERSION})")
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
