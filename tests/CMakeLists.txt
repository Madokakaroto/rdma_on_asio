find_package(Threads REQUIRED)

function(rdma_collect_backend_sources out_var)
  set(sources ${ARGN})
  if(RDMA_BACKEND STREQUAL "nd")
    list(APPEND sources ${NETWORKDIRECT_SOURCE})
  endif()
  set(${out_var} ${sources} PARENT_SCOPE)
endfunction()

function(rdma_configure_test_target name)
  target_include_directories(${name} PRIVATE
      ${CMAKE_SOURCE_DIR}/include
      ${CMAKE_SOURCE_DIR}/third_party/asio/include
      ${CMAKE_SOURCE_DIR}/tests
      ${CMAKE_SOURCE_DIR}/tests/unit
      ${CMAKE_SOURCE_DIR}/tests/benchmark
  )
  target_compile_definitions(${name} PRIVATE ASIO_STANDALONE)
  target_link_libraries(${name} PRIVATE Threads::Threads)

  if(RDMA_BACKEND STREQUAL "ibv")
    target_link_libraries(${name} PRIVATE PkgConfig::IBVERBS PkgConfig::RDMACM)
  endif()
endfunction()

function(rdma_add_backend_executable name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})
  rdma_collect_backend_sources(target_sources ${ARG_SOURCES})
  add_executable(${name} ${target_sources})
  rdma_configure_test_target(${name})
  if(ARG_LIBRARIES)
    target_link_libraries(${name} PRIVATE ${ARG_LIBRARIES})
  endif()
endfunction()

function(rdma_add_plain_executable name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  rdma_configure_test_target(${name})
  if(ARG_LIBRARIES)
    target_link_libraries(${name} PRIVATE ${ARG_LIBRARIES})
  endif()
endfunction()

function(rdma_add_ctest test_name target_name labels)
  add_test(NAME ${test_name} COMMAND ${target_name} ${ARGN})
  set_tests_properties(${test_name} PROPERTIES LABELS "${labels}")
endfunction()

function(rdma_set_hardware_ctest_properties test_name timeout)
  set_tests_properties(${test_name} PROPERTIES
      TIMEOUT ${timeout}
      RESOURCE_LOCK rdma_device
      SKIP_RETURN_CODE 77)
endfunction()

function(rdma_add_process_pair_ctest test_name labels server_target client_target)
  cmake_parse_arguments(ARG "" "TIMEOUT" "SERVER_ARGS;CLIENT_ARGS" ${ARGN})
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 45)
  endif()

  set(command
      rdma_ctest_runner
      --name ${test_name}
      --timeout-sec ${ARG_TIMEOUT}
      --work-dir ${CMAKE_CURRENT_BINARY_DIR}/ctest/${test_name}
      --server $<TARGET_FILE:${server_target}>
      --server-ready-regex "RDMA_CTEST_READY|RDMA_BENCH_READY")
  foreach(arg IN LISTS ARG_SERVER_ARGS)
    list(APPEND command --server-arg ${arg})
  endforeach()
  list(APPEND command --client $<TARGET_FILE:${client_target}>)
  foreach(arg IN LISTS ARG_CLIENT_ARGS)
    list(APPEND command --client-arg ${arg})
  endforeach()

  add_test(NAME ${test_name} COMMAND ${command})
  set_tests_properties(${test_name} PROPERTIES LABELS "${labels}")
  rdma_set_hardware_ctest_properties(${test_name} ${ARG_TIMEOUT})
endfunction()

rdma_add_backend_executable(rdma_ctest_runner
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ctest/rdma_ctest_runner.cpp)

# External ibverbs perftest baseline (upstream perftest tools: ib_send_bw, ...).
# This is a NATIVE ibv baseline, not an RDMA-on-Asio test. Single-host loopback
# smoke driven by rdma_ctest_runner: the server binds, then after a fixed readiness
# delay the client connects to the runtime-discovered local address (auto-v4 -- no
# cached address in CMake). The canonical parity command shape is produced by
# rdma_benchmark_perftest_commands; here we use a minimal smoke flag set
# (--connection RC --rdma_cm --size --iters --port). If the perftest binary is not
# installed, the case skips cleanly (RDMA_CTEST_SKIP / SKIP_RETURN_CODE 77).
function(rdma_add_external_perftest_smoke test_name tool size iters port)
  find_program(RDMA_PERFTEST_${tool} ${tool})
  if(RDMA_PERFTEST_${tool})
    add_test(NAME ${test_name}
        COMMAND rdma_ctest_runner
            --name ${test_name}
            --timeout-sec 30
            --work-dir ${CMAKE_CURRENT_BINARY_DIR}/ctest/${test_name}
            --server-ready-delay-ms 2000
            --server ${RDMA_PERFTEST_${tool}}
            --server-arg --connection --server-arg RC
            --server-arg --size --server-arg ${size}
            --server-arg --iters --server-arg ${iters}
            --server-arg --port --server-arg ${port}
            --server-arg --rdma_cm
            --client ${RDMA_PERFTEST_${tool}}
            --client-arg auto-v4
            --client-arg --connection --client-arg RC
            --client-arg --size --client-arg ${size}
            --client-arg --iters --client-arg ${iters}
            --client-arg --port --client-arg ${port}
            --client-arg --rdma_cm)
  else()
    add_test(NAME ${test_name}
        COMMAND ${CMAKE_COMMAND} -E echo
            "RDMA_CTEST_SKIP: ${tool} not found (install perftest for native ibv baselines)")
  endif()
  set_tests_properties(${test_name} PROPERTIES
      LABELS "baseline;native;perftest;ibv"
      SKIP_REGULAR_EXPRESSION "RDMA_CTEST_SKIP")
  rdma_set_hardware_ctest_properties(${test_name} 30)
endfunction()

function(rdma_add_unit_test name source)
  rdma_add_backend_executable(${name}
      SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${source})
  set(test_labels unit)
  foreach(label IN LISTS ARGN)
    list(APPEND test_labels ${label})
  endforeach()
  rdma_add_ctest(${name} ${name} "${test_labels}")
endfunction()

# Unit / compile / API-shape tests.
rdma_add_unit_test(unit_rdma_error unit/rdma/error.cpp rdma)
rdma_add_unit_test(unit_rdma_buffer unit/rdma/buffer.cpp rdma)
rdma_add_unit_test(unit_rdma_ops unit/rdma/ops.cpp rdma)
rdma_add_unit_test(unit_rdma_tcp unit/rdma/tcp.cpp rdma)
rdma_add_unit_test(unit_rdma_types unit/rdma/types.cpp rdma)
rdma_add_unit_test(unit_rdma_public_api_compile
                   unit/rdma/public_api_compile.cpp rdma compile)

if(RDMA_BACKEND STREQUAL "nd")
  rdma_add_unit_test(unit_nd_config_derive unit/nd/config_derive.cpp nd)
  rdma_add_unit_test(unit_nd_error unit/nd/error.cpp nd)
  rdma_add_unit_test(unit_nd_buffer unit/nd/buffer.cpp nd)
  rdma_add_unit_test(unit_nd_public_headers unit/nd/public_headers.cpp nd compile)
  rdma_add_unit_test(unit_nd_service_state_guards
                     unit/nd/service_state_guards.cpp nd)

  function(rdma_add_nd_asio_macro_test name test_source asio_source)
    rdma_add_plain_executable(${name}
        SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/${test_source}
            ${CMAKE_CURRENT_SOURCE_DIR}/${asio_source})
    if(MSVC)
      set_property(TARGET ${name} PROPERTY
          MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    rdma_add_ctest(${name} ${name} "unit;nd;compile;asio_macro")
  endfunction()

  rdma_add_nd_asio_macro_test(unit_nd_asio_separate_autolink
                              unit/nd/asio_separate_autolink.cpp
                              unit/asio_separate_autolink_src.cpp)

  rdma_add_nd_asio_macro_test(unit_nd_asio_separate_manual_link
                              unit/nd/asio_separate_manual_link.cpp
                              unit/asio_separate_manual_link_src.cpp)
  target_link_libraries(unit_nd_asio_separate_manual_link PRIVATE
      ${NETWORKDIRECT_LIB_NAME}
      ws2_32
      mswsock
      bcrypt)
elseif(RDMA_BACKEND STREQUAL "ibv")
  rdma_add_unit_test(unit_ibv_config_derive unit/ibv/config_derive.cpp ibv)
  rdma_add_unit_test(unit_ibv_error_helpers unit/ibv/error_helpers.cpp ibv)
  rdma_add_unit_test(unit_ibv_buffer unit/ibv/buffer.cpp ibv)
  rdma_add_unit_test(unit_ibv_public_headers unit/ibv/public_headers.cpp ibv compile)
  rdma_add_unit_test(unit_ibv_service_state_guards
                     unit/ibv/service_state_guards.cpp ibv)
endif()

# Backend smoke/integration binaries for the active backend.
math(EXPR RDMA_TEST_WAIT_DISCONNECT_PORT "${RDMA_TEST_BASE_PORT} + 7")
math(EXPR RDMA_TEST_DISCONNECT_CANCEL_PORT "${RDMA_TEST_BASE_PORT} + 12")
math(EXPR RDMA_TEST_CONTROL_CANCEL_PORT "${RDMA_TEST_BASE_PORT} + 40")
math(EXPR RDMA_NATIVE_ECHO_PORT "${RDMA_TEST_BASE_PORT} + 150")
math(EXPR RDMA_NATIVE_ECHO_POLL_PORT "${RDMA_TEST_BASE_PORT} + 151")

if(RDMA_BACKEND STREQUAL "nd")
  foreach(nd_test
          test_nd_global_init
          test_nd_device_manager
          test_nd_config_derive
          test_nd_error
          test_nd_use_device
          test_nd_refactored_compile
          test_nd_connector_listener
          test_nd_echo
          test_nd_echo_poll
          test_nd_wait_disconnect
          test_nd_disconnect_cancel
          test_nd_control_cancel)
    rdma_add_backend_executable(${nd_test}
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/nd/${nd_test}.cpp)
  endforeach()

  rdma_add_ctest(test_nd_global_init test_nd_global_init "unit;nd")
  rdma_add_ctest(test_nd_config_derive test_nd_config_derive "unit;nd")
  rdma_add_ctest(test_nd_error test_nd_error "unit;nd")
  rdma_add_ctest(test_nd_refactored_compile test_nd_refactored_compile "unit;nd;compile")
  rdma_add_ctest(integration_nd_device_manager test_nd_device_manager "unit;nd")
  rdma_add_ctest(integration_nd_use_device test_nd_use_device "unit;nd")
  rdma_add_ctest(integration_nd_connector_listener
                 test_nd_connector_listener "unit;nd")
  rdma_add_ctest(integration_nd_wait_disconnect
                 test_nd_wait_disconnect "unit;nd;race"
                 ${RDMA_TEST_WAIT_DISCONNECT_PORT})
  rdma_add_ctest(integration_nd_disconnect_cancel
                 test_nd_disconnect_cancel "unit;nd;race"
                 ${RDMA_TEST_DISCONNECT_CANCEL_PORT})
  rdma_add_ctest(integration_nd_control_cancel
                 test_nd_control_cancel "unit;nd;race"
                 ${RDMA_TEST_CONTROL_CANCEL_PORT})
  rdma_add_process_pair_ctest(native_nd_echo_event_two_process
                              "native;echo;nd"
                              test_nd_echo test_nd_echo
                              TIMEOUT 45
                              SERVER_ARGS --server --port ${RDMA_NATIVE_ECHO_PORT}
                              CLIENT_ARGS --client auto-v4 --port ${RDMA_NATIVE_ECHO_PORT})
  rdma_add_process_pair_ctest(native_nd_echo_poll_two_process
                              "native;echo;nd"
                              test_nd_echo_poll test_nd_echo_poll
                              TIMEOUT 45
                              SERVER_ARGS --server --port ${RDMA_NATIVE_ECHO_POLL_PORT}
                              CLIENT_ARGS --client auto-v4 --port ${RDMA_NATIVE_ECHO_POLL_PORT})
elseif(RDMA_BACKEND STREQUAL "ibv")
  foreach(ibv_test
          test_ibv_link
          test_ibv_device_manager
          test_ibv_connector_listener
          test_ibv_echo
          test_ibv_echo_poll
          test_ibv_wait_disconnect
          test_ibv_disconnect_cancel
          test_ibv_control_cancel)
    rdma_add_backend_executable(${ibv_test}
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ibv/${ibv_test}.cpp)
  endforeach()

  rdma_add_ctest(test_ibv_link test_ibv_link "unit;ibv;compile")
  rdma_add_ctest(integration_ibv_device_manager test_ibv_device_manager "unit;ibv")
  rdma_add_ctest(integration_ibv_connector_listener
                 test_ibv_connector_listener "unit;ibv")
  rdma_add_ctest(integration_ibv_wait_disconnect
                 test_ibv_wait_disconnect "unit;ibv;race"
                 ${RDMA_TEST_WAIT_DISCONNECT_PORT})
  rdma_add_ctest(integration_ibv_disconnect_cancel
                 test_ibv_disconnect_cancel "unit;ibv;race"
                 ${RDMA_TEST_DISCONNECT_CANCEL_PORT})
  rdma_add_ctest(integration_ibv_control_cancel
                 test_ibv_control_cancel "unit;ibv;race"
                 ${RDMA_TEST_CONTROL_CANCEL_PORT})
  rdma_add_process_pair_ctest(native_ibv_echo_event_two_process
                              "native;echo;ibv"
                              test_ibv_echo test_ibv_echo
                              TIMEOUT 45
                              SERVER_ARGS --server --port ${RDMA_NATIVE_ECHO_PORT}
                              CLIENT_ARGS --client auto-v4 --port ${RDMA_NATIVE_ECHO_PORT})
  rdma_add_process_pair_ctest(native_ibv_echo_poll_two_process
                              "native;echo;ibv"
                              test_ibv_echo_poll test_ibv_echo_poll
                              TIMEOUT 45
                              SERVER_ARGS --server --port ${RDMA_NATIVE_ECHO_POLL_PORT}
                              CLIENT_ARGS --client auto-v4 --port ${RDMA_NATIVE_ECHO_POLL_PORT})

  # External ibverbs perftest baselines (skip cleanly if perftest is not installed).
  # bw smoke: 4096B; lat smoke: 64B. Distinct ports (base+180..185).
  math(EXPR RDMA_PERFTEST_SEND_BW_PORT  "${RDMA_TEST_BASE_PORT} + 180")
  math(EXPR RDMA_PERFTEST_SEND_LAT_PORT "${RDMA_TEST_BASE_PORT} + 181")
  math(EXPR RDMA_PERFTEST_WRITE_BW_PORT "${RDMA_TEST_BASE_PORT} + 182")
  math(EXPR RDMA_PERFTEST_WRITE_LAT_PORT "${RDMA_TEST_BASE_PORT} + 183")
  math(EXPR RDMA_PERFTEST_READ_BW_PORT  "${RDMA_TEST_BASE_PORT} + 184")
  math(EXPR RDMA_PERFTEST_READ_LAT_PORT "${RDMA_TEST_BASE_PORT} + 185")
  rdma_add_external_perftest_smoke(perftest_ib_send_bw_smoke  ib_send_bw  4096 1000 ${RDMA_PERFTEST_SEND_BW_PORT})
  rdma_add_external_perftest_smoke(perftest_ib_send_lat_smoke ib_send_lat 64   1000 ${RDMA_PERFTEST_SEND_LAT_PORT})
  rdma_add_external_perftest_smoke(perftest_ib_write_bw_smoke ib_write_bw 4096 1000 ${RDMA_PERFTEST_WRITE_BW_PORT})
  rdma_add_external_perftest_smoke(perftest_ib_write_lat_smoke ib_write_lat 64  1000 ${RDMA_PERFTEST_WRITE_LAT_PORT})
  rdma_add_external_perftest_smoke(perftest_ib_read_bw_smoke  ib_read_bw  4096 1000 ${RDMA_PERFTEST_READ_BW_PORT})
  rdma_add_external_perftest_smoke(perftest_ib_read_lat_smoke ib_read_lat 64   1000 ${RDMA_PERFTEST_READ_LAT_PORT})
endif()

# Backend-agnostic rdma_* API tests.
foreach(rdma_test
        test_rdma_error
        test_rdma_echo
        test_rdma_echo_poll
        test_rdma_dual_family_echo
        test_rdma_sgl
        test_rdma_private_data
        test_rdma_regression)
  rdma_add_backend_executable(${rdma_test}
      SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/rdma/${rdma_test}.cpp)
endforeach()

rdma_add_ctest(test_rdma_error test_rdma_error "unit;rdma")

math(EXPR RDMA_TEST_SGL_PORT "${RDMA_TEST_BASE_PORT} + 60")
math(EXPR RDMA_TEST_PRIVATE_DATA_PORT "${RDMA_TEST_BASE_PORT} + 80")
math(EXPR RDMA_TEST_REGRESSION_PORT "${RDMA_TEST_BASE_PORT} + 120")
math(EXPR RDMA_TEST_ECHO_PORT "${RDMA_TEST_BASE_PORT} + 170")
math(EXPR RDMA_TEST_ECHO_POLL_PORT "${RDMA_TEST_BASE_PORT} + 171")
math(EXPR RDMA_TEST_DUAL_FAMILY_ECHO_PORT "${RDMA_TEST_BASE_PORT} + 172")

rdma_add_ctest(integration_rdma_sgl
               test_rdma_sgl "unit;rdma"
               ${RDMA_TEST_SGL_PORT})
rdma_add_ctest(integration_rdma_private_data
               test_rdma_private_data "unit;rdma"
               ${RDMA_TEST_PRIVATE_DATA_PORT})
rdma_add_ctest(integration_rdma_regression
               test_rdma_regression "unit;rdma"
               ${RDMA_TEST_REGRESSION_PORT})
rdma_add_process_pair_ctest(integration_rdma_echo_event_two_process
                            "unit;integration;echo;rdma"
                            test_rdma_echo test_rdma_echo
                            TIMEOUT 45
                            SERVER_ARGS --server --port ${RDMA_TEST_ECHO_PORT}
                            CLIENT_ARGS --client auto-v4 --port ${RDMA_TEST_ECHO_PORT})
rdma_add_process_pair_ctest(integration_rdma_echo_poll_two_process
                            "unit;integration;echo;rdma"
                            test_rdma_echo_poll test_rdma_echo_poll
                            TIMEOUT 45
                            SERVER_ARGS --server --port ${RDMA_TEST_ECHO_POLL_PORT}
                            CLIENT_ARGS --client auto-v4 --port ${RDMA_TEST_ECHO_POLL_PORT})
rdma_add_process_pair_ctest(integration_rdma_dual_family_echo_two_process
                            "unit;integration;echo;rdma"
                            test_rdma_dual_family_echo
                            test_rdma_dual_family_echo
                            TIMEOUT 60
                            SERVER_ARGS --server --port ${RDMA_TEST_DUAL_FAMILY_ECHO_PORT}
                            CLIENT_ARGS --client-v4 auto-v4 --client-v6 auto-v6 --port ${RDMA_TEST_DUAL_FAMILY_ECHO_PORT})

# Benchmark and stress helpers.
function(rdma_configure_benchmark_target name)
  rdma_configure_test_target(${name})
  target_compile_definitions(${name} PRIVATE
      RDMA_BENCH_BACKEND="${RDMA_BACKEND}"
      RDMA_BENCH_BUILD_TYPE="$<CONFIG>"
  )
endfunction()

function(rdma_add_benchmark name source)
  rdma_collect_backend_sources(benchmark_sources ${source})
  add_executable(${name} ${benchmark_sources})
  rdma_configure_benchmark_target(${name})
endfunction()

function(rdma_add_benchmark_tool name source)
  rdma_collect_backend_sources(tool_sources ${source})
  add_executable(${name} ${tool_sources})
  rdma_configure_benchmark_target(${name})
endfunction()

rdma_add_benchmark(rdma_benchmark_capability_probe
                   ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/tools/capability_probe.cpp)
rdma_add_benchmark_tool(rdma_benchmark_compare_results
                        ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/tools/compare_results.cpp)
rdma_add_benchmark_tool(rdma_benchmark_perftest_commands
                        ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/tools/perftest_commands.cpp)
rdma_add_benchmark_tool(rdma_benchmark_parse_perftest
                        ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/tools/parse_perftest.cpp)
rdma_add_benchmark_tool(rdma_benchmark_run_scenario
                        ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/tools/run_scenario.cpp)

set(RDMA_TOOLING_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/ctest/tooling)
set(RDMA_BENCHMARK_SAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/samples)

rdma_add_ctest(tooling_capability_probe_smoke
               rdma_benchmark_capability_probe "tooling;performance"
               --json-out ${RDMA_TOOLING_OUTPUT_DIR}/capability_probe.json)
set_tests_properties(tooling_capability_probe_smoke
    PROPERTIES TIMEOUT 30 RESOURCE_LOCK rdma_device)

rdma_add_ctest(tooling_perftest_commands_smoke
               rdma_benchmark_perftest_commands "tooling;performance;perftest"
               --operation send_recv
               --metric bandwidth
               --mode event
               --message-size 64
               --iterations 32
               --queue-depth 4
               --port ${RDMA_TEST_BASE_PORT}
               --json-out ${RDMA_TOOLING_OUTPUT_DIR}/perftest_commands.json)
set_tests_properties(tooling_perftest_commands_smoke PROPERTIES TIMEOUT 30)

rdma_add_ctest(tooling_parse_perftest_smoke
               rdma_benchmark_parse_perftest "tooling;performance;perftest"
               --operation send_recv
               --metric bandwidth
               --message-size 64
               --iterations 32
               --queue-depth 4
               --raw-stdout ${RDMA_BENCHMARK_SAMPLE_DIR}/perftest_send_bw_stdout.txt
               --json-out ${RDMA_TOOLING_OUTPUT_DIR}/parsed_perftest.json)
set_tests_properties(tooling_parse_perftest_smoke PROPERTIES TIMEOUT 30)

rdma_add_ctest(tooling_compare_results_smoke
               rdma_benchmark_compare_results "tooling;performance"
               ${RDMA_BENCHMARK_SAMPLE_DIR}/rdma_bench_result.json)
set_tests_properties(tooling_compare_results_smoke PROPERTIES TIMEOUT 30)

rdma_add_ctest(tooling_run_scenario_dry_run
               rdma_benchmark_run_scenario "tooling;performance;scenario"
               --scenario ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/scenarios/smoke.json
               --json-out ${RDMA_TOOLING_OUTPUT_DIR}/scenario_dry_run.json
               --output-dir ${RDMA_TOOLING_OUTPUT_DIR}/scenario_dry_run)
set_tests_properties(tooling_run_scenario_dry_run PROPERTIES TIMEOUT 30)

if(NOT WIN32)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/tools/run_comparison.sh
      ${CMAKE_CURRENT_BINARY_DIR}/run_comparison.sh
      COPYONLY)
endif()

add_library(asio_perftest_obj OBJECT
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/send_recv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/read_write.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/asio_perftest.cpp)
rdma_configure_benchmark_target(asio_perftest_obj)

set(ASIO_PERFTEST_ENTRIES ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/entries)
function(rdma_add_asio_perftest name entry)
  set(asio_perftest_sources ${entry} $<TARGET_OBJECTS:asio_perftest_obj>)
  if(RDMA_BACKEND STREQUAL "nd")
    list(APPEND asio_perftest_sources ${NETWORKDIRECT_SOURCE})
  endif()

  add_executable(${name} ${asio_perftest_sources})
  rdma_configure_benchmark_target(${name})
endfunction()

rdma_add_asio_perftest(asio_perftest  ${ASIO_PERFTEST_ENTRIES}/asio_perftest_main.cpp)
rdma_add_asio_perftest(asio_send_bw   ${ASIO_PERFTEST_ENTRIES}/asio_send_bw.cpp)
rdma_add_asio_perftest(asio_send_lat  ${ASIO_PERFTEST_ENTRIES}/asio_send_lat.cpp)
rdma_add_asio_perftest(asio_write_bw  ${ASIO_PERFTEST_ENTRIES}/asio_write_bw.cpp)
rdma_add_asio_perftest(asio_write_lat ${ASIO_PERFTEST_ENTRIES}/asio_write_lat.cpp)
rdma_add_asio_perftest(asio_read_bw   ${ASIO_PERFTEST_ENTRIES}/asio_read_bw.cpp)
rdma_add_asio_perftest(asio_read_lat  ${ASIO_PERFTEST_ENTRIES}/asio_read_lat.cpp)

rdma_add_ctest(tooling_run_scenario_execute_smoke
               rdma_benchmark_run_scenario "tooling;performance;scenario"
               --scenario ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/scenarios/smoke.json
               --execute
               --json-out ${RDMA_TOOLING_OUTPUT_DIR}/scenario_execute.json
               --output-dir ${RDMA_TOOLING_OUTPUT_DIR}/scenario_execute)
rdma_set_hardware_ctest_properties(tooling_run_scenario_execute_smoke 45)

math(EXPR RDMA_BENCH_SEND_RECV_PORT "${RDMA_TEST_BASE_PORT} + 300")
math(EXPR RDMA_BENCH_WRITE_PORT "${RDMA_TEST_BASE_PORT} + 310")
math(EXPR RDMA_BENCH_READ_PORT "${RDMA_TEST_BASE_PORT} + 320")
math(EXPR RDMA_BENCH_SEND_RECV_POLL_PORT "${RDMA_TEST_BASE_PORT} + 301")
math(EXPR RDMA_BENCH_SEND_LAT_EVENT_PORT "${RDMA_TEST_BASE_PORT} + 330")
math(EXPR RDMA_BENCH_SEND_LAT_POLL_PORT "${RDMA_TEST_BASE_PORT} + 331")
math(EXPR RDMA_BENCH_WRITE_LAT_EVENT_PORT "${RDMA_TEST_BASE_PORT} + 340")
math(EXPR RDMA_BENCH_WRITE_LAT_POLL_PORT "${RDMA_TEST_BASE_PORT} + 341")
math(EXPR RDMA_BENCH_READ_LAT_EVENT_PORT "${RDMA_TEST_BASE_PORT} + 350")
math(EXPR RDMA_BENCH_READ_LAT_POLL_PORT "${RDMA_TEST_BASE_PORT} + 351")

function(rdma_add_asio_perftest_smoke test_name target labels port mode token_type iterations queue_depth)
  rdma_add_ctest(${test_name}
                 ${target} "${labels}"
                 --single-process
                 --port ${port}
                 --mode ${mode}
                 --token-type ${token_type}
                 --topology single_host_same_process
                 --message-size 64
                 --iterations ${iterations}
                 --queue-depth ${queue_depth}
                 --timeout-sec 30)
  rdma_set_hardware_ctest_properties(${test_name} 30)
endfunction()

rdma_add_ctest(performance_asio_send_bw_single_host_smoke
               asio_send_bw "performance;rdma;bandwidth"
               --single-process
               --port ${RDMA_BENCH_SEND_RECV_PORT}
               --mode event --token-type callback
               --topology single_host_same_process
               --message-size 64 --iterations 32 --queue-depth 4)
set_tests_properties(performance_asio_send_bw_single_host_smoke
    PROPERTIES TIMEOUT 30 RESOURCE_LOCK rdma_device)

rdma_add_asio_perftest_smoke(performance_asio_send_bw_poll_single_host_smoke
                             asio_send_bw
                             "performance;rdma;bandwidth"
                             ${RDMA_BENCH_SEND_RECV_POLL_PORT}
                             poll use_future 32 4)

rdma_add_ctest(performance_asio_write_bw_single_host_smoke
               asio_write_bw "performance;rdma;bandwidth"
               --single-process
               --port ${RDMA_BENCH_WRITE_PORT}
               --mode event --token-type callback
               --topology single_host_same_process
               --message-size 64 --iterations 32 --queue-depth 4)
set_tests_properties(performance_asio_write_bw_single_host_smoke
    PROPERTIES TIMEOUT 30 RESOURCE_LOCK rdma_device)

rdma_add_ctest(performance_asio_write_bw_poll_single_host_smoke
               asio_write_bw "performance;rdma;bandwidth"
               --single-process
               --port ${RDMA_BENCH_WRITE_PORT}
               --mode poll --token-type use_future
               --topology single_host_same_process
               --message-size 64 --iterations 32 --queue-depth 4)
set_tests_properties(performance_asio_write_bw_poll_single_host_smoke
    PROPERTIES TIMEOUT 30 RESOURCE_LOCK rdma_device)

rdma_add_ctest(performance_asio_read_bw_single_host_smoke
               asio_read_bw "performance;rdma;bandwidth"
               --single-process
               --port ${RDMA_BENCH_READ_PORT}
               --mode event --token-type callback
               --topology single_host_same_process
               --message-size 64 --iterations 32 --queue-depth 4)
set_tests_properties(performance_asio_read_bw_single_host_smoke
    PROPERTIES TIMEOUT 30 RESOURCE_LOCK rdma_device)

rdma_add_ctest(performance_asio_read_bw_poll_single_host_smoke
               asio_read_bw "performance;rdma;bandwidth"
               --single-process
               --port ${RDMA_BENCH_READ_PORT}
               --mode poll --token-type use_future
               --topology single_host_same_process
               --message-size 64 --iterations 32 --queue-depth 4)
set_tests_properties(performance_asio_read_bw_poll_single_host_smoke
    PROPERTIES TIMEOUT 30 RESOURCE_LOCK rdma_device)

rdma_add_asio_perftest_smoke(performance_asio_send_lat_event_single_host_smoke
                             asio_send_lat
                             "performance;rdma;latency"
                             ${RDMA_BENCH_SEND_LAT_EVENT_PORT}
                             event callback 64 1)
rdma_add_asio_perftest_smoke(performance_asio_send_lat_poll_single_host_smoke
                             asio_send_lat
                             "performance;rdma;latency"
                             ${RDMA_BENCH_SEND_LAT_POLL_PORT}
                             poll use_future 64 1)
rdma_add_asio_perftest_smoke(performance_asio_write_lat_event_single_host_smoke
                             asio_write_lat
                             "performance;rdma;latency"
                             ${RDMA_BENCH_WRITE_LAT_EVENT_PORT}
                             event callback 64 1)
rdma_add_asio_perftest_smoke(performance_asio_write_lat_poll_single_host_smoke
                             asio_write_lat
                             "performance;rdma;latency"
                             ${RDMA_BENCH_WRITE_LAT_POLL_PORT}
                             poll use_future 64 1)
rdma_add_asio_perftest_smoke(performance_asio_read_lat_event_single_host_smoke
                             asio_read_lat
                             "performance;rdma;latency"
                             ${RDMA_BENCH_READ_LAT_EVENT_PORT}
                             event callback 64 1)
rdma_add_asio_perftest_smoke(performance_asio_read_lat_poll_single_host_smoke
                             asio_read_lat
                             "performance;rdma;latency"
                             ${RDMA_BENCH_READ_LAT_POLL_PORT}
                             poll use_future 64 1)

rdma_add_benchmark(rdma_shared_cq_stress
                   ${CMAKE_CURRENT_SOURCE_DIR}/stress/rdma/shared_cq.cpp)
rdma_add_benchmark(rdma_connect_disconnect_soak
                   ${CMAKE_CURRENT_SOURCE_DIR}/stress/rdma/connect_disconnect_soak.cpp)

math(EXPR RDMA_STRESS_SHARED_CQ_PORT "${RDMA_TEST_BASE_PORT} + 400")
math(EXPR RDMA_STRESS_SOAK_PORT "${RDMA_TEST_BASE_PORT} + 430")

rdma_add_ctest(stress_rdma_shared_cq_smoke
               rdma_shared_cq_stress "stress;rdma"
               --port ${RDMA_STRESS_SHARED_CQ_PORT}
               --mode event
               --message-size 64
               --iterations 8
               --qps 2
               --threads 2
               --timeout-sec 30)
set_tests_properties(stress_rdma_shared_cq_smoke
    PROPERTIES TIMEOUT 45 RESOURCE_LOCK rdma_device)

rdma_add_ctest(stress_rdma_connect_disconnect_soak_smoke
               rdma_connect_disconnect_soak "stress;rdma"
               --port ${RDMA_STRESS_SOAK_PORT}
               --iterations 8
               --threads 2
               --timeout-sec 30)
set_tests_properties(stress_rdma_connect_disconnect_soak_smoke
    PROPERTIES TIMEOUT 45 RESOURCE_LOCK rdma_device)

if(RDMA_BACKEND STREQUAL "nd")
  set(ND_PERFTEST_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/native/nd/nd_perftest.cpp)
  configure_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/benchmark/tools/run_nd_comparison.ps1
      ${CMAKE_CURRENT_BINARY_DIR}/run_nd_comparison.ps1
      COPYONLY)

  function(rdma_add_nd_perftest name)
    add_executable(${name}
        ${ND_PERFTEST_SOURCE}
        ${NETWORKDIRECT_SOURCE})
    rdma_configure_benchmark_target(${name})
  endfunction()

  function(rdma_add_nd_perftest_entry name operation metric)
    rdma_add_nd_perftest(${name})
    target_compile_definitions(${name} PRIVATE
        ND_PERFTEST_PRESET_OPERATION="${operation}"
        ND_PERFTEST_PRESET_METRIC="${metric}")
  endfunction()

  rdma_add_nd_perftest(nd_perftest)
  rdma_add_nd_perftest_entry(nd_send_bw   send_recv bandwidth)
  rdma_add_nd_perftest_entry(nd_send_lat  send_recv latency)
  rdma_add_nd_perftest_entry(nd_write_bw  write     bandwidth)
  rdma_add_nd_perftest_entry(nd_write_lat write     latency)
  rdma_add_nd_perftest_entry(nd_read_bw   read      bandwidth)
  rdma_add_nd_perftest_entry(nd_read_lat  read      latency)

  rdma_add_nd_perftest(native_nd_send_recv_bench)
  rdma_add_nd_perftest(native_nd_read_write_bench)

  math(EXPR RDMA_NATIVE_ND_PORT "${RDMA_TEST_BASE_PORT} + 500")
  math(EXPR RDMA_NATIVE_ND_WRITE_PORT "${RDMA_TEST_BASE_PORT} + 510")
  math(EXPR RDMA_NATIVE_ND_READ_PORT "${RDMA_TEST_BASE_PORT} + 520")
  math(EXPR RDMA_NATIVE_ND_SEND_LAT_PORT "${RDMA_TEST_BASE_PORT} + 530")
  math(EXPR RDMA_NATIVE_ND_WRITE_LAT_PORT "${RDMA_TEST_BASE_PORT} + 540")
  math(EXPR RDMA_NATIVE_ND_READ_LAT_PORT "${RDMA_TEST_BASE_PORT} + 550")
  math(EXPR RDMA_NATIVE_ND_GENERIC_PORT "${RDMA_TEST_BASE_PORT} + 560")
  math(EXPR RDMA_NATIVE_ND_GENERIC_WRITE_PORT "${RDMA_TEST_BASE_PORT} + 570")
  math(EXPR RDMA_NATIVE_ND_GENERIC_READ_PORT "${RDMA_TEST_BASE_PORT} + 580")

  rdma_add_ctest(baseline_native_nd_send_recv_smoke
                 nd_send_bw "baseline;native;native_nd;nd;bandwidth"
                 --single-process
                 --port ${RDMA_NATIVE_ND_PORT}
                 --mode poll
                 --topology single_host_same_process
                 --message-size 64
                 --iterations 32
                 --queue-depth 4)
  set_tests_properties(baseline_native_nd_send_recv_smoke
      PROPERTIES TIMEOUT 30 RESOURCE_LOCK rdma_device)

  rdma_add_ctest(baseline_native_nd_write_smoke
                 nd_write_bw "baseline;native;native_nd;nd;bandwidth"
                 --single-process
                 --port ${RDMA_NATIVE_ND_WRITE_PORT}
                 --mode poll
                 --topology single_host_same_process
                 --message-size 64
                 --iterations 32
                 --queue-depth 4)
  set_tests_properties(baseline_native_nd_write_smoke
      PROPERTIES TIMEOUT 30 RESOURCE_LOCK rdma_device)

  rdma_add_ctest(baseline_native_nd_read_smoke
                 nd_read_bw "baseline;native;native_nd;nd;bandwidth"
                 --single-process
                 --port ${RDMA_NATIVE_ND_READ_PORT}
                 --mode poll
                 --topology single_host_same_process
                 --message-size 64
                 --iterations 32
                 --queue-depth 4)
  set_tests_properties(baseline_native_nd_read_smoke
      PROPERTIES TIMEOUT 30 RESOURCE_LOCK rdma_device)

  rdma_add_asio_perftest_smoke(baseline_native_nd_send_lat_smoke
                               nd_send_lat
                               "baseline;native;native_nd;nd;latency"
                               ${RDMA_NATIVE_ND_SEND_LAT_PORT}
                               poll use_future 64 1)
  rdma_add_asio_perftest_smoke(baseline_native_nd_write_lat_smoke
                               nd_write_lat
                               "baseline;native;native_nd;nd;latency"
                               ${RDMA_NATIVE_ND_WRITE_LAT_PORT}
                               poll use_future 64 1)
  rdma_add_asio_perftest_smoke(baseline_native_nd_read_lat_smoke
                               nd_read_lat
                               "baseline;native;native_nd;nd;latency"
                               ${RDMA_NATIVE_ND_READ_LAT_PORT}
                               poll use_future 64 1)

  rdma_add_ctest(baseline_native_nd_perftest_generic_smoke
                 nd_perftest "baseline;native;native_nd;nd;tooling"
                 --single-process
                 --port ${RDMA_NATIVE_ND_GENERIC_PORT}
                 --operation send_recv
                 --metric bandwidth
                 --mode poll
                 --topology single_host_same_process
                 --message-size 64
                 --iterations 32
                 --queue-depth 4)
  rdma_set_hardware_ctest_properties(baseline_native_nd_perftest_generic_smoke 30)

  rdma_add_ctest(baseline_native_nd_send_recv_bench_compat_smoke
                 native_nd_send_recv_bench "baseline;native;native_nd;nd;tooling"
                 --single-process
                 --port ${RDMA_NATIVE_ND_GENERIC_WRITE_PORT}
                 --operation send_recv
                 --metric bandwidth
                 --mode poll
                 --topology single_host_same_process
                 --message-size 64
                 --iterations 32
                 --queue-depth 4)
  rdma_set_hardware_ctest_properties(baseline_native_nd_send_recv_bench_compat_smoke 30)

  rdma_add_ctest(baseline_native_nd_read_write_bench_compat_smoke
                 native_nd_read_write_bench "baseline;native;native_nd;nd;tooling"
                 --single-process
                 --port ${RDMA_NATIVE_ND_GENERIC_READ_PORT}
                 --operation write
                 --metric bandwidth
                 --mode poll
                 --topology single_host_same_process
                 --message-size 64
                 --iterations 32
                 --queue-depth 4)
  rdma_set_hardware_ctest_properties(baseline_native_nd_read_write_bench_compat_smoke 30)
endif()

# --- Separate-compilation verification (ASIO_SEPARATE_COMPILATION) ---
# One src TU (#include <rdma/impl/src.hpp>) + two TUs that include the public
# headers + main. Building this target proves ODR (no duplicate non-template
# definitions across TUs) and linkage (every split symbol resolves from the src
# TU); running it is the smoke. The whole target is compiled with
# ASIO_SEPARATE_COMPILATION (whole-program single-mode). See
# docs/separate_compilation_usage.md / docs/plans/separate_compilation_plan.md.
rdma_add_backend_executable(unit_rdma_separate_compilation
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/sepcomp/src_rdma.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sepcomp/tu_a.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sepcomp/tu_b.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sepcomp/main.cpp)
target_compile_definitions(unit_rdma_separate_compilation PRIVATE ASIO_SEPARATE_COMPILATION)
rdma_add_ctest(unit_rdma_separate_compilation unit_rdma_separate_compilation "unit;sepcomp")
