#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "rdma_bench_common.hpp"

namespace fs = std::filesystem;

std::string perftest_tool(rdma_bench::operation_kind operation,
                          rdma_bench::metric_kind metric) {
  bool latency = metric == rdma_bench::metric_kind::latency;
  switch (operation) {
    case rdma_bench::operation_kind::send_recv:
      return latency ? "ib_send_lat" : "ib_send_bw";
    case rdma_bench::operation_kind::write:
      return latency ? "ib_write_lat" : "ib_write_bw";
    case rdma_bench::operation_kind::read:
      return latency ? "ib_read_lat" : "ib_read_bw";
  }
  return {};
}

std::string tool_path(std::string const& tool, std::string const& bin_dir) {
  if (bin_dir.empty()) return tool;
  return (fs::path(bin_dir) / tool).string();
}

std::string build_common_args(rdma_bench::options const& opt) {
  std::ostringstream os;
  os << " --connection RC"
     << " --size " << opt.message_size
     << " --port " << opt.port;
  // READ/ATOMIC verbs reject --inline_size (even 0): "Inline feature not
  // available on READ/Atomic verbs". Emit it only for send/write.
  if (opt.operation != rdma_bench::operation_kind::read) {
    os << " --inline_size " << opt.inline_size;
  }
  if (opt.metric == rdma_bench::metric_kind::latency) {
    os << " --iters " << opt.iterations;
  } else if (opt.duration_sec > 0.0) {
    os << " --duration " << static_cast<int>(opt.duration_sec);
  } else {
    os << " --iters " << opt.iterations;
  }
  if (opt.metric == rdma_bench::metric_kind::bandwidth) {
    os << " --tx-depth " << opt.queue_depth;
    // READ rx-depth can only be 1 (the reader side posts no recv); perftest
    // rejects a larger value ("rx depth can be only 1"). Only send/write take it.
    if (opt.operation != rdma_bench::operation_kind::read) {
      os << " --rx-depth " << opt.queue_depth;
    }
    os << " --cq-mod " << opt.cq_mod;
  }
  if (opt.mode == "event") os << " --events";
  os << " --rdma_cm";
  return os.str();
}

int main(int argc, char* argv[]) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv, false);
    auto tool = perftest_tool(opt.operation, opt.metric);
    auto exe = tool_path(tool, opt.perftest_bin_dir);
    auto args = build_common_args(opt);

    std::ostringstream server;
    server << rdma_bench::quote_arg(exe) << args;
    std::ostringstream client;
    client << rdma_bench::quote_arg(exe) << " "
           << rdma_bench::quote_arg(opt.peer_addr.empty() ? "<server-ip>"
                                                         : opt.peer_addr)
           << args;

    std::ostringstream json;
    json << "{\n";
    json << "  \"schema_version\": \"1\",\n";
    json << "  \"baseline\": \"perftest\",\n";
    json << "  \"operation\": \""
         << rdma_bench::operation_name(opt.operation) << "\",\n";
    json << "  \"metric\": \"" << rdma_bench::metric_name(opt.metric)
         << "\",\n";
    json << "  \"tool\": \"" << tool << "\",\n";
    json << "  \"parity_constraints\": [\"cq_mod_" << opt.cq_mod
         << "\", \"inline_size_" << opt.inline_size
         << "\", \"rdma_cm\"],\n";
    json << "  \"server_command\": \""
         << rdma_bench::json_escape(server.str()) << "\",\n";
    json << "  \"client_command\": \""
         << rdma_bench::json_escape(client.str()) << "\"\n";
    json << "}\n";

    if (!opt.json_out.empty()) {
      auto parent = fs::path(opt.json_out).parent_path();
      if (!parent.empty()) fs::create_directories(parent);
      std::ofstream out(opt.json_out, std::ios::binary);
      if (!out) throw std::runtime_error("failed to open json output");
      out << json.str();
    }
    std::cout << json.str();
    return 0;
  } catch (std::runtime_error const& e) {
    if (std::string_view(e.what()) == "help") {
      rdma_bench::print_usage(argv[0]);
      return 0;
    }
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
