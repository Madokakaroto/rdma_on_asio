add_executable(test_ibv_link test_ibv_link.cpp)

target_include_directories(test_ibv_link PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_ibv_link PRIVATE ASIO_STANDALONE)
target_link_libraries(test_ibv_link PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)
