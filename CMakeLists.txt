cmake_minimum_required(VERSION 3.20)
project(rdma_on_asio VERSION 0.1.0 LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RDMA_BUILD_UNIT_TESTS "Build deterministic unit tests" ON)
option(RDMA_BUILD_INTEGRATION_TESTS "Build smoke and integration tests" ON)
option(RDMA_ENABLE_HARDWARE_TESTS "Register tests that need RDMA hardware" OFF)
set(RDMA_TEST_ADDR "" CACHE STRING "Local RDMA-capable IP address for hardware tests")
set(RDMA_TEST_BASE_PORT "5000" CACHE STRING "Base TCP/RDMA port for hardware tests")

# RDMA backend detection
if(WIN32)
  set(RDMA_BACKEND "nd" CACHE STRING "RDMA backend (nd, ibv)")
else()
  set(RDMA_BACKEND "ibv" CACHE STRING "RDMA backend (nd, ibv)")
endif()

# NetworkDirect backend
if(RDMA_BACKEND STREQUAL "nd")
  set(ND_DIR ${CMAKE_SOURCE_DIR}/third_party/networkdirect/src/ndutil)
  set(ND_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated/networkdirect)
  file(MAKE_DIRECTORY ${ND_GENERATED_DIR})

  find_program(MC_EXE mc.exe
      HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  )

  add_custom_command(
      OUTPUT ${ND_GENERATED_DIR}/ndstatus.h
      COMMAND ${MC_EXE} -h ${ND_GENERATED_DIR} ${ND_DIR}/ndstatus.mc
      DEPENDS ${ND_DIR}/ndstatus.mc
      COMMENT "Generating ndstatus.h from ndstatus.mc"
  )

  add_library(ndutil STATIC
      ${ND_DIR}/ndaddr.cpp
      ${ND_DIR}/ndfrmwrk.cpp
      ${ND_DIR}/ndprov.cpp
      ${ND_GENERATED_DIR}/ndstatus.h
  )
  target_include_directories(ndutil PUBLIC ${ND_DIR} ${ND_GENERATED_DIR})
  target_link_libraries(ndutil PUBLIC ws2_32)
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
