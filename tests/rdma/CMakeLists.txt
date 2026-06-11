# Cross-platform tests: written against the backend-agnostic rdma_* API.
# Built for whichever backend is active; link the matching backend libs.
find_package(Threads REQUIRED)

# echo (event-driven, shared service CQ) and echo_poll (user-owned poll-mode CQ
# spun on a dedicated thread) share build settings; only the poll variant needs
# Threads, but linking it to both is harmless.
foreach(rdma_test test_rdma_echo test_rdma_echo_poll test_rdma_sgl)
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
