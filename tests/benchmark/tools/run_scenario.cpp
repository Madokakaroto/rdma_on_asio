#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "rdma_bench_common.hpp"

namespace fs = std::filesystem;

std::string executable_suffix() {
#ifdef _WIN32
  return ".exe";
#else
  return "";
#endif
}

std::string rdma_tool_name(rdma_bench::operation_kind operation) {
  // Stage 9a: the unified asio_perftest binary multiplexes all operations via
  // --operation (already emitted in common_args), so one target serves every
  // scenario. The legacy rdma_send_recv_bench / rdma_read_write_bench split is
  // gone; the per-operation entrypoints asio_{send,read,write}_{bw,lat} exist for
  // perftest-identical command lines but are not needed by the scenario runner.
  (void)operation;
  return "asio_perftest";
}

std::string resolve_tool(char const* argv0, std::string const& tool) {
  auto dir = fs::absolute(argv0).parent_path();
  auto candidate = dir / (tool + executable_suffix());
  std::error_code ec;
  if (fs::exists(candidate, ec)) return candidate.string();
  return tool;
}

std::string common_args(rdma_bench::options const& opt) {
  std::ostringstream os;
  os << " --operation " << rdma_bench::operation_name(opt.operation)
     << " --metric " << rdma_bench::metric_name(opt.metric)
     << " --mode " << opt.mode
     << " --token-type " << opt.token_type
     << " --topology " << opt.topology
     << " --message-size " << opt.message_size
     << " --iterations " << opt.iterations
     << " --queue-depth " << opt.queue_depth
     << " --qps " << opt.qps
     << " --threads " << opt.threads
     << " --port " << opt.port
     << " --timeout-sec " << opt.timeout_sec;
  return os.str();
}

int run_redirected(std::string const& command, fs::path const& stdout_path,
                   fs::path const& stderr_path) {
  auto full = command + " > " + rdma_bench::quote_arg(stdout_path.string()) +
              " 2> " + rdma_bench::quote_arg(stderr_path.string());
  return std::system(full.c_str());
}

void write_text(fs::path const& path, std::string const& text) {
  auto parent = path.parent_path();
  if (!parent.empty()) fs::create_directories(parent);
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("failed to open " + path.string());
  out << text;
}

int main(int argc, char* argv[]) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv, false);
    auto tool = resolve_tool(argv[0], rdma_tool_name(opt.operation));
    auto args = common_args(opt);

    fs::path output_dir =
        opt.output_dir.empty()
            ? fs::path("benchmark-run") / opt.scenario_name
            : fs::path(opt.output_dir);
    fs::create_directories(output_dir);

    auto result_json = output_dir / "rdma_on_asio.json";
    auto stdout_log = output_dir / "rdma_on_asio.stdout.log";
    auto stderr_log = output_dir / "rdma_on_asio.stderr.log";

    std::ostringstream single;
    single << rdma_bench::quote_arg(tool)
           << " --single-process --local-addr "
           << rdma_bench::quote_arg(opt.local_addr.empty() ? "<local-rdma-ip>"
                                                        : opt.local_addr)
           << args << " --json-out " << rdma_bench::quote_arg(result_json.string());

    std::ostringstream server;
    server << rdma_bench::quote_arg(tool) << " --server";
    if (!opt.local_addr.empty()) {
      server << " --local-addr " << rdma_bench::quote_arg(opt.local_addr);
    }
    server << args;

    std::ostringstream client;
    client << rdma_bench::quote_arg(tool) << " --client "
           << rdma_bench::quote_arg(opt.peer_addr.empty() ? "<server-rdma-ip>"
                                                        : opt.peer_addr)
           << args;

    int exit_code = 0;
    bool executed = false;
    auto begin = std::chrono::steady_clock::now();
    if (opt.execute) {
      if (opt.local_addr.empty()) {
        throw std::invalid_argument("--execute requires --local-addr");
      }
      if (opt.topology != "single_host_same_process") {
        throw std::invalid_argument(
            "--execute currently runs single_host_same_process only; use the "
            "manifest server/client commands for two-process or multi-host runs");
      }
      executed = true;
      exit_code = run_redirected(single.str(), stdout_log, stderr_log);
    }
    auto end = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration<double>(end - begin).count();

    std::ostringstream json;
    json << "{\n";
    json << "  \"schema_version\": \"1\",\n";
    json << "  \"scenario_name\": \""
         << rdma_bench::json_escape(opt.scenario_name) << "\",\n";
    json << "  \"operation\": \""
         << rdma_bench::operation_name(opt.operation) << "\",\n";
    json << "  \"metric\": \"" << rdma_bench::metric_name(opt.metric)
         << "\",\n";
    json << "  \"topology\": \"" << rdma_bench::json_escape(opt.topology)
         << "\",\n";
    json << "  \"executed\": " << (executed ? "true" : "false") << ",\n";
    json << "  \"exit_code\": " << exit_code << ",\n";
    json << "  \"elapsed_sec\": " << std::fixed << std::setprecision(6)
         << elapsed << ",\n";
    json << "  \"output_dir\": \"" << rdma_bench::json_escape(output_dir.string())
         << "\",\n";
    json << "  \"artifacts\": {\n";
    json << "    \"result_json\": \""
         << rdma_bench::json_escape(result_json.string()) << "\",\n";
    json << "    \"stdout\": \"" << rdma_bench::json_escape(stdout_log.string())
         << "\",\n";
    json << "    \"stderr\": \"" << rdma_bench::json_escape(stderr_log.string())
         << "\"\n";
    json << "  },\n";
    json << "  \"commands\": {\n";
    json << "    \"single_process\": \""
         << rdma_bench::json_escape(single.str()) << "\",\n";
    json << "    \"server\": \"" << rdma_bench::json_escape(server.str())
         << "\",\n";
    json << "    \"client\": \"" << rdma_bench::json_escape(client.str())
         << "\"\n";
    json << "  },\n";
    json << "  \"runner_note\": \"--execute runs only same-process local "
            "benchmarks; two-process and multi-host runs intentionally keep "
            "explicit server/client commands for now\"\n";
    json << "}\n";

    auto manifest = opt.json_out.empty() ? output_dir / "manifest.json"
                                         : fs::path(opt.json_out);
    write_text(manifest, json.str());
    std::cout << json.str();
    return exit_code == 0 ? 0 : 1;
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
