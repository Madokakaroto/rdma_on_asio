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
