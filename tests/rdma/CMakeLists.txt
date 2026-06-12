# Cross-platform tests: written against the backend-agnostic rdma_* API.
# Built for whichever backend is active; link the matching backend libs.
find_package(Threads REQUIRED)

# echo (event-driven, shared service CQ) and echo_poll (user-owned poll-mode CQ
# spun on a dedicated thread) share build settings; only the poll variant needs
# Threads, but linking it to both is harmless.
foreach(rdma_test
        test_rdma_error
        test_rdma_echo
        test_rdma_echo_poll
        test_rdma_sgl
        test_rdma_private_data
        test_rdma_regression)
  add_executable(${rdma_test} ${rdma_test}.cpp)

  target_include_directories(${rdma_test} PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/third_party/asio/include
  )

  target_compile_definitions(${rdma_test} PRIVATE ASIO_STANDALONE)
  target_link_libraries(${rdma_test} PRIVATE Threads::Threads)

  if(RDMA_BACKEND STREQUAL "ibv")
    target_link_libraries(${rdma_test} PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)
  elseif(RDMA_BACKEND STREQUAL "nd")
    target_compile_definitions(${rdma_test} PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(${rdma_test} PRIVATE ndutil)
  endif()
endforeach()

add_test(NAME test_rdma_error COMMAND test_rdma_error)
set_tests_properties(test_rdma_error PROPERTIES LABELS "unit;rdma;legacy")

if(RDMA_ENABLE_HARDWARE_TESTS AND RDMA_TEST_ADDR)
  math(EXPR RDMA_TEST_SGL_PORT "${RDMA_TEST_BASE_PORT} + 60")
  math(EXPR RDMA_TEST_PRIVATE_DATA_PORT "${RDMA_TEST_BASE_PORT} + 80")
  math(EXPR RDMA_TEST_REGRESSION_PORT "${RDMA_TEST_BASE_PORT} + 120")

  add_test(NAME integration_rdma_sgl
           COMMAND test_rdma_sgl ${RDMA_TEST_ADDR} ${RDMA_TEST_SGL_PORT})
  set_tests_properties(integration_rdma_sgl
      PROPERTIES LABELS "integration;hardware;rdma")

  add_test(NAME integration_rdma_private_data
           COMMAND test_rdma_private_data
                   ${RDMA_TEST_ADDR} ${RDMA_TEST_PRIVATE_DATA_PORT})
  set_tests_properties(integration_rdma_private_data
      PROPERTIES LABELS "integration;hardware;rdma")

  add_test(NAME integration_rdma_regression
           COMMAND test_rdma_regression
                   ${RDMA_TEST_ADDR} ${RDMA_TEST_REGRESSION_PORT})
  set_tests_properties(integration_rdma_regression
      PROPERTIES LABELS "integration;hardware;rdma")
endif()
