add_executable(test_nd_global_init test_nd_global_init.cpp)

target_include_directories(test_nd_global_init PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_global_init PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_global_init PRIVATE ndutil)

add_executable(test_nd_device_manager test_nd_device_manager.cpp)

target_include_directories(test_nd_device_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_device_manager PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_device_manager PRIVATE ndutil)

add_executable(test_nd_config_derive test_nd_config_derive.cpp)

target_include_directories(test_nd_config_derive PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_config_derive PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_config_derive PRIVATE ndutil)

add_executable(test_nd_error test_nd_error.cpp)

target_include_directories(test_nd_error PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_error PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_error PRIVATE ndutil)

add_executable(test_nd_use_device test_nd_use_device.cpp)

target_include_directories(test_nd_use_device PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_use_device PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_use_device PRIVATE ndutil)

add_executable(test_nd_refactored_compile test_nd_refactored_compile.cpp)

target_include_directories(test_nd_refactored_compile PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_refactored_compile PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_refactored_compile PRIVATE ndutil)

add_executable(test_nd_connector_listener test_nd_connector_listener.cpp)

target_include_directories(test_nd_connector_listener PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_connector_listener PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_connector_listener PRIVATE ndutil)

add_executable(test_nd_echo test_nd_echo.cpp)

target_include_directories(test_nd_echo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_echo PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_echo PRIVATE ndutil)

# Poll-mode echo: data plane runs against a user-owned CQ spun on its own thread.
find_package(Threads REQUIRED)

add_executable(test_nd_echo_poll test_nd_echo_poll.cpp)

target_include_directories(test_nd_echo_poll PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_echo_poll PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_echo_poll PRIVATE ndutil Threads::Threads)

add_executable(test_nd_wait_disconnect test_nd_wait_disconnect.cpp)

target_include_directories(test_nd_wait_disconnect PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_wait_disconnect PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_wait_disconnect PRIVATE ndutil)

add_executable(test_nd_disconnect_cancel test_nd_disconnect_cancel.cpp)

target_include_directories(test_nd_disconnect_cancel PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_disconnect_cancel PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_disconnect_cancel PRIVATE ndutil Threads::Threads)

add_executable(test_nd_control_cancel test_nd_control_cancel.cpp)

target_include_directories(test_nd_control_cancel PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_control_cancel PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_control_cancel PRIVATE ndutil Threads::Threads)

add_test(NAME test_nd_global_init COMMAND test_nd_global_init)
set_tests_properties(test_nd_global_init PROPERTIES LABELS "unit;nd;legacy")
add_test(NAME test_nd_device_manager COMMAND test_nd_device_manager)
set_tests_properties(test_nd_device_manager
    PROPERTIES LABELS "integration;nd;legacy")
add_test(NAME test_nd_config_derive COMMAND test_nd_config_derive)
set_tests_properties(test_nd_config_derive PROPERTIES LABELS "unit;nd;legacy")
add_test(NAME test_nd_error COMMAND test_nd_error)
set_tests_properties(test_nd_error PROPERTIES LABELS "unit;nd;legacy")
add_test(NAME test_nd_use_device COMMAND test_nd_use_device)
set_tests_properties(test_nd_use_device
    PROPERTIES LABELS "integration;nd;legacy")
add_test(NAME test_nd_refactored_compile COMMAND test_nd_refactored_compile)
set_tests_properties(test_nd_refactored_compile
    PROPERTIES LABELS "compile;nd;legacy")
add_test(NAME test_nd_connector_listener COMMAND test_nd_connector_listener)
set_tests_properties(test_nd_connector_listener
    PROPERTIES LABELS "integration;nd;legacy")

if(RDMA_ENABLE_HARDWARE_TESTS AND RDMA_TEST_ADDR)
  math(EXPR RDMA_TEST_WAIT_DISCONNECT_PORT "${RDMA_TEST_BASE_PORT} + 7")
  math(EXPR RDMA_TEST_DISCONNECT_CANCEL_PORT "${RDMA_TEST_BASE_PORT} + 12")
  math(EXPR RDMA_TEST_CONTROL_CANCEL_PORT "${RDMA_TEST_BASE_PORT} + 40")

  add_test(NAME integration_nd_wait_disconnect
           COMMAND test_nd_wait_disconnect
                   ${RDMA_TEST_ADDR} ${RDMA_TEST_WAIT_DISCONNECT_PORT})
  set_tests_properties(integration_nd_wait_disconnect
      PROPERTIES LABELS "integration;hardware;race;nd")

  add_test(NAME integration_nd_disconnect_cancel
           COMMAND test_nd_disconnect_cancel
                   ${RDMA_TEST_ADDR} ${RDMA_TEST_DISCONNECT_CANCEL_PORT})
  set_tests_properties(integration_nd_disconnect_cancel
      PROPERTIES LABELS "integration;hardware;race;nd")

  add_test(NAME integration_nd_control_cancel
           COMMAND test_nd_control_cancel
                   ${RDMA_TEST_ADDR} ${RDMA_TEST_CONTROL_CANCEL_PORT})
  set_tests_properties(integration_nd_control_cancel
      PROPERTIES LABELS "integration;hardware;race;nd")
endif()
