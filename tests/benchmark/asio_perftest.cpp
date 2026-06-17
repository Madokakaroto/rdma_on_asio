// Unified asio_perftest entry point. The two verb modules (send_recv.cpp,
// read_write.cpp) expose run_send_recv / run_read_write; this TU owns option
// parsing, operation routing, and the shared CLI used by both the multiplexed
// `asio_perftest` tool and the perftest-shaped entrypoints (asio_send_bw, ...).
//
// asio_perftest mirrors perftest's command line and run structure; it differs
// only in that the data path goes through the RDMA-on-Asio public API instead of
// raw verbs, so the measured delta against perftest is the abstraction cost.

#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "rdma_bench_common.hpp"
#include "asio_perftest_core.hpp"

namespace asio_perftest {

int dispatch(rdma_bench::options opt, std::string command_line) {
  switch (opt.operation) {
    case rdma_bench::operation_kind::send_recv:
      return run_send_recv(std::move(opt), std::move(command_line));
    case rdma_bench::operation_kind::write:
    case rdma_bench::operation_kind::read:
      return run_read_write(std::move(opt), std::move(command_line));
  }
  auto r = rdma_bench::make_skip_result(opt, command_line, "unknown operation",
                                        "operation_unknown");
  rdma_bench::write_result(r, opt.json_out);
  return 0;
}

int cli_main(int argc, char** argv, char const* preset_op,
             char const* preset_metric) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv);
    auto cmd = rdma_bench::command_line(argc, argv);
    if (preset_op) opt.operation = rdma_bench::parse_operation(preset_op);
    if (preset_metric) {
      opt.metric = std::string_view(preset_metric) == "latency"
                       ? rdma_bench::metric_kind::latency
                       : rdma_bench::metric_kind::bandwidth;
    }
    // Re-apply the latency=one-outstanding rule after the preset entrypoints
    // (asio_*_lat) set the metric -- parse_options ran before the preset, so its
    // own latency->qd=1 clamp did not see the preset metric.
    if (opt.metric == rdma_bench::metric_kind::latency) opt.queue_depth = 1;
    // Stage 9b duration mode: margin trims the ramp from both ends, so 2*margin
    // must be strictly less than the window or nothing is measured. Reject early
    // with a clear skip.
    if (opt.duration_sec > 0.0 && 2.0 * opt.margin_sec >= opt.duration_sec) {
      auto r = rdma_bench::make_skip_result(
          opt, cmd, "2*margin >= duration leaves an empty measured window",
          "margin_too_large");
      rdma_bench::write_result(r, opt.json_out);
      return 0;
    }
    // Duration mode: the server serves for the whole window, so its watchdog /
    // poll timeout must outlive duration + 2*margin (the poll-mode send/recv
    // server gates its poll_until on timeout_sec). Bump it if the user's value
    // is too small.
    if (opt.duration_sec > 0.0) {
      auto const need = static_cast<std::uint32_t>(
                            opt.duration_sec + 2.0 * opt.margin_sec) +
                        15u;
      if (opt.timeout_sec < need) opt.timeout_sec = need;
    }
    // Not-implemented gate: perftest knobs that need rdma-on-asio interface/impl
    // work which is out of scope this iteration (inline_size / cq-mod / post_list
    // selective signaling + WR batching). The flags parse for command-line parity,
    // but a non-default value cannot be honored through the current public API, so
    // emit a not_implemented skip instead of a misleading number. (--connection
    // non-RC already throws at parse.)
    if (auto reason = rdma_bench::not_implemented_reason(opt); !reason.empty()) {
      auto r = rdma_bench::make_skip_result(opt, cmd, reason, "not_implemented");
      rdma_bench::write_result(r, opt.json_out);
      return 0;  // a capability skip is not a process failure
    }
    return dispatch(std::move(opt), std::move(cmd));
  } catch (std::runtime_error const& e) {
    if (std::string_view(e.what()) == "help") {
      rdma_bench::print_usage(argv[0]);
      return 0;
    }
    std::cerr << "fatal: " << e.what() << "\n";
    rdma_bench::print_usage(argv[0]);
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    rdma_bench::print_usage(argv[0]);
    return 1;
  }
}

}  // namespace asio_perftest
