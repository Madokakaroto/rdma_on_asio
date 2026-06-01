# Cross-platform tests: written against the backend-agnostic rdma_* API.
# Built for whichever backend is active; link the matching backend libs.
add_executable(test_rdma_echo test_rdma_echo.cpp)

target_include_directories(test_rdma_echo PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/asio/include
)

target_compile_definitions(test_rdma_echo PRIVATE ASIO_STANDALONE)

if(RDMA_BACKEND STREQUAL "ibv")
  target_link_libraries(test_rdma_echo PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)
elseif(RDMA_BACKEND STREQUAL "nd")
  target_compile_definitions(test_rdma_echo PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(test_rdma_echo PRIVATE ndutil)
endif()
