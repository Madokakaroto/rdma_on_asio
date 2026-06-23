find_package(Threads REQUIRED)

function(rdma_add_unit_test name source)
  set(test_sources ${source})
  if(RDMA_BACKEND STREQUAL "nd")
    list(APPEND test_sources ${NETWORKDIRECT_SOURCE})
  endif()

  add_executable(${name} ${test_sources})

  target_include_directories(${name} PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/third_party/asio/include
      ${CMAKE_SOURCE_DIR}/tests/unit
  )

  target_compile_definitions(${name} PRIVATE ASIO_STANDALONE)
  target_link_libraries(${name} PRIVATE Threads::Threads)

  if(RDMA_BACKEND STREQUAL "nd")
    rdma_configure_nd_backend(${name})
  elseif(RDMA_BACKEND STREQUAL "ibv")
    target_link_libraries(${name} PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)
  endif()

  add_test(NAME ${name} COMMAND ${name})

  set(test_labels unit)
  foreach(label IN LISTS ARGN)
    list(APPEND test_labels ${label})
  endforeach()
  set_tests_properties(${name} PROPERTIES LABELS "${test_labels}")
endfunction()

rdma_add_unit_test(unit_rdma_error rdma/error.cpp rdma)
rdma_add_unit_test(unit_rdma_buffer rdma/buffer.cpp rdma)
rdma_add_unit_test(unit_rdma_ops rdma/ops.cpp rdma)
rdma_add_unit_test(unit_rdma_tcp rdma/tcp.cpp rdma)
rdma_add_unit_test(unit_rdma_types rdma/types.cpp rdma)
rdma_add_unit_test(unit_rdma_public_api_compile
                   rdma/public_api_compile.cpp rdma compile)

if(RDMA_BACKEND STREQUAL "nd")
  rdma_add_unit_test(unit_nd_config_derive nd/config_derive.cpp nd)
  rdma_add_unit_test(unit_nd_error nd/error.cpp nd)
  rdma_add_unit_test(unit_nd_buffer nd/buffer.cpp nd)
  rdma_add_unit_test(unit_nd_public_headers nd/public_headers.cpp nd compile)
  rdma_add_unit_test(unit_nd_service_state_guards
                     nd/service_state_guards.cpp nd)

  if(RDMA_BUILD_ASIO_MACRO_TESTS)
    function(rdma_add_nd_asio_macro_test name test_source asio_source)
      add_executable(${name}
          ${test_source}
          ${asio_source})
      target_include_directories(${name} PRIVATE
          ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_SOURCE_DIR}/third_party/asio/include
          ${CMAKE_SOURCE_DIR}/tests/unit
      )
      target_compile_definitions(${name} PRIVATE ASIO_STANDALONE)
      target_link_libraries(${name} PRIVATE Threads::Threads)
      rdma_configure_nd_backend(${name})
      if(MSVC)
        set_property(TARGET ${name} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
      endif()
      add_test(NAME ${name} COMMAND ${name})
      set_tests_properties(${name}
          PROPERTIES LABELS "unit;nd;compile;asio_macro")
    endfunction()

    rdma_add_nd_asio_macro_test(unit_nd_asio_separate_autolink
                                nd/asio_separate_autolink.cpp
                                asio_separate_autolink_src.cpp)

    rdma_add_nd_asio_macro_test(unit_nd_asio_separate_manual_link
                                nd/asio_separate_manual_link.cpp
                                asio_separate_manual_link_src.cpp)
    target_link_libraries(unit_nd_asio_separate_manual_link PRIVATE
        ${NETWORKDIRECT_LIB_NAME}
        ws2_32
        mswsock
        bcrypt)
  endif()
elseif(RDMA_BACKEND STREQUAL "ibv")
  rdma_add_unit_test(unit_ibv_config_derive ibv/config_derive.cpp ibv)
  rdma_add_unit_test(unit_ibv_error_helpers ibv/error_helpers.cpp ibv)
  rdma_add_unit_test(unit_ibv_buffer ibv/buffer.cpp ibv)
  rdma_add_unit_test(unit_ibv_public_headers ibv/public_headers.cpp ibv compile)
  rdma_add_unit_test(unit_ibv_service_state_guards
                     ibv/service_state_guards.cpp ibv)
endif()
