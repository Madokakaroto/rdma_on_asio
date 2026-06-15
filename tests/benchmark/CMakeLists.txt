find_package(Threads REQUIRED)

function(rdma_configure_benchmark_target name)
  target_include_directories(${name} PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/third_party/asio/include
      ${CMAKE_SOURCE_DIR}/tests/benchmark
  )

  target_compile_definitions(${name} PRIVATE
      ASIO_STANDALONE
      RDMA_BENCH_BACKEND="${RDMA_BACKEND}"
      RDMA_BENCH_BUILD_TYPE="$<CONFIG>"
  )
  target_link_libraries(${name} PRIVATE Threads::Threads)

  if(RDMA_BACKEND STREQUAL "nd")
    target_compile_definitions(${name} PRIVATE _WIN32_WINNT=0x0A00)
    target_link_libraries(${name} PRIVATE ndutil)
  elseif(RDMA_BACKEND STREQUAL "ibv")
    target_link_libraries(${name} PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)
  endif()
endfunction()

function(rdma_add_benchmark name source)
  add_executable(${name} ${source})
  rdma_configure_benchmark_target(${name})
endfunction()

function(rdma_add_benchmark_tool name source)
  add_executable(${name} ${source})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests/benchmark)
  target_compile_definitions(${name} PRIVATE
      ASIO_STANDALONE
      RDMA_BENCH_BACKEND="${RDMA_BACKEND}"
      RDMA_BENCH_BUILD_TYPE="$<CONFIG>"
  )
  target_link_libraries(${name} PRIVATE Threads::Threads)
endfunction()

# Tools are useful for every stress/performance/native workflow.
rdma_add_benchmark(rdma_benchmark_capability_probe
                   ${CMAKE_SOURCE_DIR}/tests/benchmark/tools/capability_probe.cpp)
rdma_add_benchmark_tool(rdma_benchmark_compare_results
                        ${CMAKE_SOURCE_DIR}/tests/benchmark/tools/compare_results.cpp)
rdma_add_benchmark_tool(rdma_benchmark_perftest_commands
                        ${CMAKE_SOURCE_DIR}/tests/benchmark/tools/perftest_commands.cpp)
rdma_add_benchmark_tool(rdma_benchmark_parse_perftest
                        ${CMAKE_SOURCE_DIR}/tests/benchmark/tools/parse_perftest.cpp)
rdma_add_benchmark_tool(rdma_benchmark_run_scenario
                        ${CMAKE_SOURCE_DIR}/tests/benchmark/tools/run_scenario.cpp)

if(RDMA_BUILD_PERFORMANCE_TESTS)
  rdma_add_benchmark(rdma_send_recv_bench
                     ${CMAKE_SOURCE_DIR}/tests/performance/rdma/send_recv.cpp)
  rdma_add_benchmark(rdma_read_write_bench
                     ${CMAKE_SOURCE_DIR}/tests/performance/rdma/read_write.cpp)

  if(RDMA_ENABLE_HARDWARE_TESTS AND RDMA_TEST_ADDR)
    math(EXPR RDMA_BENCH_SEND_RECV_PORT "${RDMA_TEST_BASE_PORT} + 300")
    math(EXPR RDMA_BENCH_WRITE_PORT "${RDMA_TEST_BASE_PORT} + 310")
    math(EXPR RDMA_BENCH_READ_PORT "${RDMA_TEST_BASE_PORT} + 320")

    add_test(NAME performance_rdma_send_recv_single_host_smoke
             COMMAND rdma_send_recv_bench
                     --single-process
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_SEND_RECV_PORT}
                     --metric bandwidth
                     --mode event
                     --token-type callback
                     --topology single_host_same_process
                     --message-size 64
                     --iterations 32
                     --queue-depth 4)
    set_tests_properties(performance_rdma_send_recv_single_host_smoke
        PROPERTIES
          LABELS "performance;latency;hardware;manual;rdma"
          TIMEOUT 30)

    add_test(NAME performance_rdma_write_single_host_smoke
             COMMAND rdma_read_write_bench
                     --single-process
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_WRITE_PORT}
                     --operation write
                     --metric bandwidth
                     --mode event
                     --token-type callback
                     --topology single_host_same_process
                     --message-size 64
                     --iterations 32
                     --queue-depth 4)
    set_tests_properties(performance_rdma_write_single_host_smoke
        PROPERTIES
          LABELS "performance;hardware;manual;rdma"
          TIMEOUT 30)

    add_test(NAME performance_rdma_write_poll_single_host_smoke
             COMMAND rdma_read_write_bench
                     --single-process
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_WRITE_PORT}
                     --operation write
                     --metric bandwidth
                     --mode poll
                     --token-type use_future
                     --topology single_host_same_process
                     --message-size 64
                     --iterations 32
                     --queue-depth 4)
    set_tests_properties(performance_rdma_write_poll_single_host_smoke
        PROPERTIES
          LABELS "performance;hardware;manual;rdma"
          TIMEOUT 30)

    add_test(NAME performance_rdma_read_single_host_smoke
             COMMAND rdma_read_write_bench
                     --single-process
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_READ_PORT}
                     --operation read
                     --metric bandwidth
                     --mode event
                     --token-type callback
                     --topology single_host_same_process
                     --message-size 64
                     --iterations 32
                     --queue-depth 4)
    set_tests_properties(performance_rdma_read_single_host_smoke
        PROPERTIES
          LABELS "performance;hardware;manual;rdma"
          TIMEOUT 30)

    add_test(NAME performance_rdma_read_poll_single_host_smoke
             COMMAND rdma_read_write_bench
                     --single-process
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_READ_PORT}
                     --operation read
                     --metric bandwidth
                     --mode poll
                     --token-type use_future
                     --topology single_host_same_process
                     --message-size 64
                     --iterations 32
                     --queue-depth 4)
    set_tests_properties(performance_rdma_read_poll_single_host_smoke
        PROPERTIES
          LABELS "performance;hardware;manual;rdma"
          TIMEOUT 30)
  endif()
endif()

if(RDMA_BUILD_STRESS_TESTS)
  rdma_add_benchmark(rdma_shared_cq_stress
                     ${CMAKE_SOURCE_DIR}/tests/stress/rdma/shared_cq.cpp)
  rdma_add_benchmark(rdma_connect_disconnect_soak
                     ${CMAKE_SOURCE_DIR}/tests/stress/rdma/connect_disconnect_soak.cpp)

  if(RDMA_ENABLE_HARDWARE_TESTS AND RDMA_TEST_ADDR)
    math(EXPR RDMA_STRESS_SHARED_CQ_PORT "${RDMA_TEST_BASE_PORT} + 400")
    math(EXPR RDMA_STRESS_SOAK_PORT "${RDMA_TEST_BASE_PORT} + 430")

    add_test(NAME stress_rdma_shared_cq_smoke
             COMMAND rdma_shared_cq_stress
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_STRESS_SHARED_CQ_PORT}
                     --mode event
                     --message-size 64
                     --iterations 8
                     --qps 2
                     --threads 2
                     --timeout-sec 30)
    set_tests_properties(stress_rdma_shared_cq_smoke
        PROPERTIES LABELS "stress;hardware;manual;rdma" TIMEOUT 45)

    add_test(NAME stress_rdma_connect_disconnect_soak_smoke
             COMMAND rdma_connect_disconnect_soak
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_STRESS_SOAK_PORT}
                     --iterations 8
                     --threads 2
                     --timeout-sec 30)
    set_tests_properties(stress_rdma_connect_disconnect_soak_smoke
        PROPERTIES LABELS "stress;hardware;manual;rdma" TIMEOUT 45)
  endif()
endif()

if(RDMA_BUILD_NATIVE_BASELINES)
  rdma_add_benchmark(native_nd_send_recv_bench
                     ${CMAKE_SOURCE_DIR}/tests/native/nd/native_nd_baseline.cpp)
  rdma_add_benchmark(native_nd_read_write_bench
                     ${CMAKE_SOURCE_DIR}/tests/native/nd/native_nd_baseline.cpp)

  if(RDMA_ENABLE_HARDWARE_TESTS AND RDMA_TEST_ADDR)
    math(EXPR RDMA_NATIVE_ND_PORT "${RDMA_TEST_BASE_PORT} + 500")
    math(EXPR RDMA_NATIVE_ND_WRITE_PORT "${RDMA_TEST_BASE_PORT} + 510")
    math(EXPR RDMA_NATIVE_ND_READ_PORT "${RDMA_TEST_BASE_PORT} + 520")
    add_test(NAME baseline_native_nd_send_recv_smoke
             COMMAND native_nd_send_recv_bench
                     --single-process
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_NATIVE_ND_PORT}
                     --operation send_recv
                     --metric bandwidth
                     --mode poll
                     --topology single_host_same_process
                     --message-size 64
                     --iterations 32
                     --queue-depth 4)
    set_tests_properties(baseline_native_nd_send_recv_smoke
        PROPERTIES LABELS "baseline;native_nd;nd;hardware;manual" TIMEOUT 30)

    add_test(NAME baseline_native_nd_write_smoke
             COMMAND native_nd_read_write_bench
                     --single-process
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_NATIVE_ND_WRITE_PORT}
                     --operation write
                     --metric bandwidth
                     --mode poll
                     --topology single_host_same_process
                     --message-size 64
                     --iterations 32
                     --queue-depth 4)
    set_tests_properties(baseline_native_nd_write_smoke
        PROPERTIES LABELS "baseline;native_nd;nd;hardware;manual" TIMEOUT 30)

    add_test(NAME baseline_native_nd_read_smoke
             COMMAND native_nd_read_write_bench
                     --single-process
                     --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_NATIVE_ND_READ_PORT}
                     --operation read
                     --metric bandwidth
                     --mode poll
                     --topology single_host_same_process
                     --message-size 64
                     --iterations 32
                     --queue-depth 4)
    set_tests_properties(baseline_native_nd_read_smoke
        PROPERTIES LABELS "baseline;native_nd;nd;hardware;manual" TIMEOUT 30)
  endif()
endif()
