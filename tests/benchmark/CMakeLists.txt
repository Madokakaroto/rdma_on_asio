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
  # asio_perftest: the unified, perftest-aligned benchmark (Stage 9a). The two
  # verb modules and the dispatcher compile once into an object library; the
  # multiplexed `asio_perftest` tool and the six perftest-shaped entrypoints
  # (asio_{send,read,write}_{bw,lat}) link it. Replaces the legacy
  # rdma_send_recv_bench / rdma_read_write_bench.
  add_library(asio_perftest_obj OBJECT
      ${CMAKE_SOURCE_DIR}/tests/performance/rdma/send_recv.cpp
      ${CMAKE_SOURCE_DIR}/tests/performance/rdma/read_write.cpp
      ${CMAKE_SOURCE_DIR}/tests/benchmark/asio_perftest.cpp)
  rdma_configure_benchmark_target(asio_perftest_obj)

  set(ASIO_PERFTEST_ENTRIES ${CMAKE_SOURCE_DIR}/tests/benchmark/entries)
  function(rdma_add_asio_perftest name entry)
    add_executable(${name} ${entry} $<TARGET_OBJECTS:asio_perftest_obj>)
    rdma_configure_benchmark_target(${name})
  endfunction()
  rdma_add_asio_perftest(asio_perftest  ${ASIO_PERFTEST_ENTRIES}/asio_perftest_main.cpp)
  rdma_add_asio_perftest(asio_send_bw   ${ASIO_PERFTEST_ENTRIES}/asio_send_bw.cpp)
  rdma_add_asio_perftest(asio_send_lat  ${ASIO_PERFTEST_ENTRIES}/asio_send_lat.cpp)
  rdma_add_asio_perftest(asio_write_bw  ${ASIO_PERFTEST_ENTRIES}/asio_write_bw.cpp)
  rdma_add_asio_perftest(asio_write_lat ${ASIO_PERFTEST_ENTRIES}/asio_write_lat.cpp)
  rdma_add_asio_perftest(asio_read_bw   ${ASIO_PERFTEST_ENTRIES}/asio_read_bw.cpp)
  rdma_add_asio_perftest(asio_read_lat  ${ASIO_PERFTEST_ENTRIES}/asio_read_lat.cpp)

  if(RDMA_ENABLE_HARDWARE_TESTS AND RDMA_TEST_ADDR)
    math(EXPR RDMA_BENCH_SEND_RECV_PORT "${RDMA_TEST_BASE_PORT} + 300")
    math(EXPR RDMA_BENCH_WRITE_PORT "${RDMA_TEST_BASE_PORT} + 310")
    math(EXPR RDMA_BENCH_READ_PORT "${RDMA_TEST_BASE_PORT} + 320")

    add_test(NAME performance_asio_send_bw_single_host_smoke
             COMMAND asio_send_bw
                     --single-process --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_SEND_RECV_PORT}
                     --mode event --token-type callback
                     --topology single_host_same_process
                     --message-size 64 --iterations 32 --queue-depth 4)
    set_tests_properties(performance_asio_send_bw_single_host_smoke
        PROPERTIES LABELS "performance;hardware;manual;rdma" TIMEOUT 30)

    add_test(NAME performance_asio_write_bw_single_host_smoke
             COMMAND asio_write_bw
                     --single-process --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_WRITE_PORT}
                     --mode event --token-type callback
                     --topology single_host_same_process
                     --message-size 64 --iterations 32 --queue-depth 4)
    set_tests_properties(performance_asio_write_bw_single_host_smoke
        PROPERTIES LABELS "performance;hardware;manual;rdma" TIMEOUT 30)

    add_test(NAME performance_asio_write_bw_poll_single_host_smoke
             COMMAND asio_write_bw
                     --single-process --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_WRITE_PORT}
                     --mode poll --token-type use_future
                     --topology single_host_same_process
                     --message-size 64 --iterations 32 --queue-depth 4)
    set_tests_properties(performance_asio_write_bw_poll_single_host_smoke
        PROPERTIES LABELS "performance;hardware;manual;rdma" TIMEOUT 30)

    add_test(NAME performance_asio_read_bw_single_host_smoke
             COMMAND asio_read_bw
                     --single-process --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_READ_PORT}
                     --mode event --token-type callback
                     --topology single_host_same_process
                     --message-size 64 --iterations 32 --queue-depth 4)
    set_tests_properties(performance_asio_read_bw_single_host_smoke
        PROPERTIES LABELS "performance;hardware;manual;rdma" TIMEOUT 30)

    add_test(NAME performance_asio_read_bw_poll_single_host_smoke
             COMMAND asio_read_bw
                     --single-process --local-addr ${RDMA_TEST_ADDR}
                     --port ${RDMA_BENCH_READ_PORT}
                     --mode poll --token-type use_future
                     --topology single_host_same_process
                     --message-size 64 --iterations 32 --queue-depth 4)
    set_tests_properties(performance_asio_read_bw_poll_single_host_smoke
        PROPERTIES LABELS "performance;hardware;manual;rdma" TIMEOUT 30)
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
