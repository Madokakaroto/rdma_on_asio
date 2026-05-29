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

add_executable(test_nd_echo test_nd_echo.cpp)

target_include_directories(test_nd_echo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_nd_echo PRIVATE ASIO_STANDALONE _WIN32_WINNT=0x0A00)
target_link_libraries(test_nd_echo PRIVATE ndutil)
