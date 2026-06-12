add_executable(test_ibv_link test_ibv_link.cpp)

target_include_directories(test_ibv_link PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_ibv_link PRIVATE ASIO_STANDALONE)
target_link_libraries(test_ibv_link PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)

add_executable(test_ibv_device_manager test_ibv_device_manager.cpp)

target_include_directories(test_ibv_device_manager PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_ibv_device_manager PRIVATE ASIO_STANDALONE)
target_link_libraries(test_ibv_device_manager PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)

add_executable(test_ibv_connector_listener test_ibv_connector_listener.cpp)

target_include_directories(test_ibv_connector_listener PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_ibv_connector_listener PRIVATE ASIO_STANDALONE)
target_link_libraries(test_ibv_connector_listener PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)

add_executable(test_ibv_echo test_ibv_echo.cpp)

target_include_directories(test_ibv_echo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_ibv_echo PRIVATE ASIO_STANDALONE)
target_link_libraries(test_ibv_echo PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)

# Poll-mode echo: data plane runs against a user-owned CQ spun on its own thread.
find_package(Threads REQUIRED)

add_executable(test_ibv_echo_poll test_ibv_echo_poll.cpp)

target_include_directories(test_ibv_echo_poll PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_ibv_echo_poll PRIVATE ASIO_STANDALONE)
target_link_libraries(test_ibv_echo_poll PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM Threads::Threads)

add_executable(test_ibv_wait_disconnect test_ibv_wait_disconnect.cpp)

target_include_directories(test_ibv_wait_disconnect PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_ibv_wait_disconnect PRIVATE ASIO_STANDALONE)
target_link_libraries(test_ibv_wait_disconnect PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)

# MT-safe, state-adaptive disconnect (pending-connect abort + established teardown
# driven from another thread): needs Threads.
add_executable(test_ibv_disconnect_cancel test_ibv_disconnect_cancel.cpp)

target_include_directories(test_ibv_disconnect_cancel PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_ibv_disconnect_cancel PRIVATE ASIO_STANDALONE)
target_link_libraries(test_ibv_disconnect_cancel PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM Threads::Threads)

# Stage 2: per-op (cancellation_slot) cancellation of control-plane ops.
add_executable(test_ibv_control_cancel test_ibv_control_cancel.cpp)

target_include_directories(test_ibv_control_cancel PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_ibv_control_cancel PRIVATE ASIO_STANDALONE)
target_link_libraries(test_ibv_control_cancel PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM Threads::Threads)

add_test(NAME test_ibv_link COMMAND test_ibv_link)
set_tests_properties(test_ibv_link PROPERTIES LABELS "compile;ibv;legacy")

add_test(NAME test_ibv_device_manager COMMAND test_ibv_device_manager)
set_tests_properties(test_ibv_device_manager
    PROPERTIES LABELS "integration;ibv;legacy")

add_test(NAME test_ibv_connector_listener COMMAND test_ibv_connector_listener)
set_tests_properties(test_ibv_connector_listener
    PROPERTIES LABELS "integration;ibv;legacy")

if(RDMA_ENABLE_HARDWARE_TESTS AND RDMA_TEST_ADDR)
  math(EXPR RDMA_TEST_WAIT_DISCONNECT_PORT "${RDMA_TEST_BASE_PORT} + 7")
  math(EXPR RDMA_TEST_DISCONNECT_CANCEL_PORT "${RDMA_TEST_BASE_PORT} + 12")
  math(EXPR RDMA_TEST_CONTROL_CANCEL_PORT "${RDMA_TEST_BASE_PORT} + 40")

  add_test(NAME integration_ibv_wait_disconnect
           COMMAND test_ibv_wait_disconnect
                   ${RDMA_TEST_ADDR} ${RDMA_TEST_WAIT_DISCONNECT_PORT})
  set_tests_properties(integration_ibv_wait_disconnect
      PROPERTIES LABELS "integration;hardware;race;ibv")

  add_test(NAME integration_ibv_disconnect_cancel
           COMMAND test_ibv_disconnect_cancel
                   ${RDMA_TEST_ADDR} ${RDMA_TEST_DISCONNECT_CANCEL_PORT})
  set_tests_properties(integration_ibv_disconnect_cancel
      PROPERTIES LABELS "integration;hardware;race;ibv")

  add_test(NAME integration_ibv_control_cancel
           COMMAND test_ibv_control_cancel
                   ${RDMA_TEST_ADDR} ${RDMA_TEST_CONTROL_CANCEL_PORT})
  set_tests_properties(integration_ibv_control_cancel
      PROPERTIES LABELS "integration;hardware;race;ibv")
endif()
