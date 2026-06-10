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
add_test(NAME test_nd_device_manager COMMAND test_nd_device_manager)
add_test(NAME test_nd_config_derive COMMAND test_nd_config_derive)
add_test(NAME test_nd_error COMMAND test_nd_error)
add_test(NAME test_nd_use_device COMMAND test_nd_use_device)
add_test(NAME test_nd_refactored_compile COMMAND test_nd_refactored_compile)
add_test(NAME test_nd_connector_listener COMMAND test_nd_connector_listener)
