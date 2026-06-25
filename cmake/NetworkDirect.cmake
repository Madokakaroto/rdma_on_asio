include_guard(GLOBAL)

function(_rdma_networkdirect_find_mc out_var)
  find_program(_networkdirect_mc_exe mc.exe
      HINTS
          "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  )
  if(NOT _networkdirect_mc_exe)
    message(FATAL_ERROR "mc.exe is required to generate NetworkDirect ndstatus.h")
  endif()

  set(${out_var} "${_networkdirect_mc_exe}" PARENT_SCOPE)
endfunction()

function(_rdma_networkdirect_find_msbuild out_var)
  set(_networkdirect_msbuild "${CMAKE_VS_MSBUILD_COMMAND}")
  if(NOT _networkdirect_msbuild)
    find_program(_networkdirect_msbuild MSBuild.exe)
  endif()
  if(NOT _networkdirect_msbuild)
    message(FATAL_ERROR "MSBuild.exe is required to build native NetworkDirect ndutil.vcxproj")
  endif()

  set(${out_var} "${_networkdirect_msbuild}" PARENT_SCOPE)
endfunction()

function(_rdma_networkdirect_generate_headers)
  cmake_parse_arguments(
      NETWORKDIRECT_GEN
      ""
      "TARGET;OUT_INCLUDE_DIR;OUT_RESOURCE_DIR;OUT_HEADER"
      ""
      ${ARGN})

  if(NOT NETWORKDIRECT_GEN_TARGET)
    message(FATAL_ERROR "_rdma_networkdirect_generate_headers requires TARGET")
  endif()
  if(NOT NETWORKDIRECT_GEN_OUT_INCLUDE_DIR)
    message(FATAL_ERROR "_rdma_networkdirect_generate_headers requires OUT_INCLUDE_DIR")
  endif()
  if(NOT NETWORKDIRECT_GEN_OUT_RESOURCE_DIR)
    message(FATAL_ERROR "_rdma_networkdirect_generate_headers requires OUT_RESOURCE_DIR")
  endif()

  set(_networkdirect_header "${NETWORKDIRECT_GEN_OUT_INCLUDE_DIR}/ndstatus.h")
  add_custom_command(
      OUTPUT "${_networkdirect_header}"
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "${NETWORKDIRECT_GEN_OUT_INCLUDE_DIR}"
              "${NETWORKDIRECT_GEN_OUT_RESOURCE_DIR}"
      COMMAND "${NETWORKDIRECT_MC_EXE}"
              -h "${NETWORKDIRECT_GEN_OUT_INCLUDE_DIR}"
              -r "${NETWORKDIRECT_GEN_OUT_RESOURCE_DIR}"
              "${NETWORKDIRECT_NDUTIL_DIR}/ndstatus.mc"
      DEPENDS "${NETWORKDIRECT_NDUTIL_DIR}/ndstatus.mc"
      COMMENT "Generating NetworkDirect ndstatus.h"
      VERBATIM
  )

  add_custom_target(${NETWORKDIRECT_GEN_TARGET}
      DEPENDS "${_networkdirect_header}")

  if(NETWORKDIRECT_GEN_OUT_HEADER)
    set(${NETWORKDIRECT_GEN_OUT_HEADER} "${_networkdirect_header}" PARENT_SCOPE)
  endif()
endfunction()

function(_rdma_networkdirect_add_native_ndutil)
  cmake_parse_arguments(
      NETWORKDIRECT_NDUTIL
      ""
      "TARGET;GENERATED_HEADERS_TARGET;GENERATED_INCLUDE_DIR"
      ""
      ${ARGN})

  if(NOT NETWORKDIRECT_NDUTIL_TARGET)
    message(FATAL_ERROR "_rdma_networkdirect_add_native_ndutil requires TARGET")
  endif()
  if(NOT NETWORKDIRECT_NDUTIL_GENERATED_HEADERS_TARGET)
    message(FATAL_ERROR "_rdma_networkdirect_add_native_ndutil requires GENERATED_HEADERS_TARGET")
  endif()
  if(NOT NETWORKDIRECT_NDUTIL_GENERATED_INCLUDE_DIR)
    message(FATAL_ERROR "_rdma_networkdirect_add_native_ndutil requires GENERATED_INCLUDE_DIR")
  endif()

  if(NOT CMAKE_VS_PLATFORM_NAME)
    message(FATAL_ERROR
        "CMAKE_VS_PLATFORM_NAME is required to build native NetworkDirect ndutil.vcxproj")
  endif()
  if(NOT CMAKE_VS_PLATFORM_TOOLSET)
    message(FATAL_ERROR
        "CMAKE_VS_PLATFORM_TOOLSET is required to build native NetworkDirect ndutil.vcxproj")
  endif()

  set(_networkdirect_lib
      "${NETWORKDIRECT_LIB_DIR}/${NETWORKDIRECT_LIB_NAME}")
  set(_networkdirect_native_header
      "${NETWORKDIRECT_NDUTIL_GENERATED_INCLUDE_DIR}/ndstatus.h")

  add_custom_command(
      OUTPUT
          "${_networkdirect_lib}"
          "${_networkdirect_native_header}"
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "${NETWORKDIRECT_NDUTIL_GENERATED_INCLUDE_DIR}"
              "${NETWORKDIRECT_LIB_DIR}"
              "${NETWORKDIRECT_NATIVE_ROOT}/obj/ndutil"
      COMMAND "${NETWORKDIRECT_MC_EXE}"
              -h "${NETWORKDIRECT_NDUTIL_GENERATED_INCLUDE_DIR}"
              -r "${NETWORKDIRECT_NDUTIL_GENERATED_INCLUDE_DIR}"
              "${NETWORKDIRECT_NDUTIL_DIR}/ndstatus.mc"
      COMMAND "${NETWORKDIRECT_MSBUILD_EXE}"
              "${NETWORKDIRECT_NDUTIL_DIR}/ndutil.vcxproj"
              /nologo /m /t:Build
              /p:Configuration=$<CONFIG>
              /p:Platform=${CMAKE_VS_PLATFORM_NAME}
              /p:ImportDirectoryBuildProps=false
              /p:PlatformToolset=${CMAKE_VS_PLATFORM_TOOLSET}
              /p:WindowsTargetPlatformVersion=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}
              /p:OutputPath=${NETWORKDIRECT_LIB_DIR}/
              /p:OutDir=${NETWORKDIRECT_LIB_DIR}/
              /p:OutIncludePath=${NETWORKDIRECT_NDUTIL_GENERATED_INCLUDE_DIR}/
              /p:IntDir=${NETWORKDIRECT_NATIVE_ROOT}/obj/ndutil/
      DEPENDS
          "${NETWORKDIRECT_NDUTIL_DIR}/ndutil.vcxproj"
          "${CMAKE_SOURCE_DIR}/third_party/networkdirect/src/src.props"
          "${NETWORKDIRECT_NDUTIL_DIR}/ndstatus.mc"
          "${NETWORKDIRECT_NDUTIL_DIR}/ndaddr.cpp"
          "${NETWORKDIRECT_NDUTIL_DIR}/ndfrmwrk.cpp"
          "${NETWORKDIRECT_NDUTIL_DIR}/ndprov.cpp"
          "${NETWORKDIRECT_NDUTIL_DIR}/precomp.h"
      COMMENT "Building native NetworkDirect ndutil.lib"
      VERBATIM
  )

  add_custom_target(${NETWORKDIRECT_NDUTIL_TARGET}
      DEPENDS "${_networkdirect_lib}")
  add_dependencies(${NETWORKDIRECT_NDUTIL_TARGET}
      ${NETWORKDIRECT_NDUTIL_GENERATED_HEADERS_TARGET})
endfunction()

function(init_networdirect)
  set(NETWORKDIRECT_NDUTIL_DIR
      "${CMAKE_SOURCE_DIR}/third_party/networkdirect/src/ndutil")
  set(NETWORKDIRECT_GENERATED_DIR
      "${CMAKE_BINARY_DIR}/generated/networkdirect")
  set(NETWORKDIRECT_NATIVE_ROOT
      "${CMAKE_BINARY_DIR}/networkdirect-native/$<CONFIG>-${CMAKE_VS_PLATFORM_NAME}")
  set(NETWORKDIRECT_NATIVE_INCLUDE_DIR
      "${NETWORKDIRECT_NATIVE_ROOT}/include")
  set(NETWORKDIRECT_LIB_DIR
      "${NETWORKDIRECT_NATIVE_ROOT}/ndutil")
  set(NETWORKDIRECT_LIB_NAME
      "ndutil.lib")
  set(NETWORKDIRECT_INCLUDE_DIRS
      "${NETWORKDIRECT_NDUTIL_DIR}"
      "${NETWORKDIRECT_GENERATED_DIR}")

  _rdma_networkdirect_find_mc(NETWORKDIRECT_MC_EXE)
  _rdma_networkdirect_find_msbuild(NETWORKDIRECT_MSBUILD_EXE)

  _rdma_networkdirect_generate_headers(
      TARGET rdma_networkdirect_generated_headers
      OUT_INCLUDE_DIR "${NETWORKDIRECT_GENERATED_DIR}"
      OUT_RESOURCE_DIR "${NETWORKDIRECT_GENERATED_DIR}"
      OUT_HEADER NETWORKDIRECT_GENERATED_HEADER)

  add_library(rdma_networkdirect_headers INTERFACE)
  target_include_directories(rdma_networkdirect_headers INTERFACE
      ${NETWORKDIRECT_INCLUDE_DIRS})

  _rdma_networkdirect_add_native_ndutil(
      TARGET rdma_networkdirect_native_ndutil
      GENERATED_HEADERS_TARGET rdma_networkdirect_generated_headers
      GENERATED_INCLUDE_DIR "${NETWORKDIRECT_NATIVE_INCLUDE_DIR}")

  add_library(rdma_networkdirect_backend INTERFACE)
  target_compile_definitions(rdma_networkdirect_backend INTERFACE
      _WIN32_WINNT=0x0A00)
  target_include_directories(rdma_networkdirect_backend INTERFACE
      ${NETWORKDIRECT_INCLUDE_DIRS})
  target_link_directories(rdma_networkdirect_backend INTERFACE
      "${NETWORKDIRECT_LIB_DIR}")
  add_dependencies(rdma_networkdirect_backend
      rdma_networkdirect_generated_headers
      rdma_networkdirect_native_ndutil)

  link_libraries(rdma_networkdirect_backend)

  set(NETWORKDIRECT_LIB_NAME
      "${NETWORKDIRECT_LIB_NAME}"
      PARENT_SCOPE)
endfunction()
