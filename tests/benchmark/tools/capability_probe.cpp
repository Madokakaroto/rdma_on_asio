#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "asio/io_context.hpp"

#include "rdma/rdma.hpp"
#include "rdma_bench_common.hpp"

namespace fs = std::filesystem;
namespace rdma = asio::rdma;
using tcp = rdma::tcp;

std::vector<std::string> split_path(std::string const& path) {
  std::vector<std::string> out;
#ifdef _WIN32
  char const sep = ';';
#else
  char const sep = ':';
#endif
  std::size_t begin = 0;
  while (begin <= path.size()) {
    auto end = path.find(sep, begin);
    if (end == std::string::npos) end = path.size();
    if (end != begin) out.push_back(path.substr(begin, end - begin));
    begin = end + 1;
  }
  return out;
}

std::optional<fs::path> find_executable(std::string const& name,
                                        std::string const& explicit_dir) {
  std::vector<fs::path> dirs;
  if (!explicit_dir.empty()) dirs.emplace_back(explicit_dir);
  if (auto* path_env = std::getenv("PATH")) {
    for (auto const& p : split_path(path_env)) dirs.emplace_back(p);
  }

  std::vector<std::string> names{name};
#ifdef _WIN32
  if (!name.ends_with(".exe")) names.push_back(name + ".exe");
#endif

  for (auto const& dir : dirs) {
    for (auto const& candidate_name : names) {
      auto candidate = dir / candidate_name;
      std::error_code ec;
      if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
        return candidate;
      }
    }
  }
  return std::nullopt;
}

std::string tool_field(std::string const& name, std::string const& explicit_dir) {
  auto path = find_executable(name, explicit_dir);
  std::ostringstream os;
  os << "    \"" << name << "\": ";
  if (path) {
    os << "{\"available\": true, \"path\": \""
       << rdma_bench::json_escape(path->string()) << "\"}";
  } else {
    os << "{\"available\": false, \"path\": null}";
  }
  return os.str();
}

int main(int argc, char* argv[]) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv, false);
    auto cmd = rdma_bench::command_line(argc, argv);

    bool rdma_device_available = false;
    std::string rdma_error;
    try {
      asio::io_context io;
      auto device = rdma::rdma_device_manager_t::instance()
                        .get_first_available_device(tcp::v4(), {});
      rdma::use_device(io, device);
      rdma_device_available = true;
    } catch (std::exception const& e) {
      rdma_error = e.what();
    }

    std::vector<std::string> perftest_tools{
        "ib_send_bw",  "ib_send_lat",  "ib_write_bw",
        "ib_write_lat", "ib_read_bw",   "ib_read_lat",
    };

    std::ostringstream json;
    json << "{\n";
    json << "  \"schema_version\": \"1\",\n";
    json << "  \"probe\": \"rdma_benchmark_capabilities\",\n";
    json << "  \"backend\": \"" << rdma_bench::json_escape(opt.backend)
         << "\",\n";
#ifdef _WIN32
    json << "  \"platform\": \"windows\",\n";
#else
    json << "  \"platform\": \"posix\",\n";
#endif
    json << "  \"rdma_device_available\": "
         << (rdma_device_available ? "true" : "false") << ",\n";
    json << "  \"rdma_device_error\": ";
    if (rdma_error.empty()) json << "null,\n";
    else json << "\"" << rdma_bench::json_escape(rdma_error) << "\",\n";
    json << "  \"topologies\": [\"single_host_same_process\", "
            "\"single_host_two_process\", \"two_host_direct\", "
            "\"two_host_switch\"],\n";
    json << "  \"rdma_on_asio\": {\n";
    json << "    \"send_recv_event\": true,\n";
    json << "    \"send_recv_poll\": true,\n";
    json << "    \"read_write_event\": true,\n";
    json << "    \"read_write_poll\": true\n";
    json << "  },\n";
    json << "  \"native_nd_baseline\": {\n";
#ifdef _WIN32
    json << "    \"build_target_available\": true,\n";
    json << "    \"send_recv_direct_data_path_available\": true,\n";
    json << "    \"read_write_direct_data_path_available\": true,\n";
    json << "    \"direct_data_path_available\": true\n";
#else
    json << "    \"build_target_available\": false,\n";
    json << "    \"send_recv_direct_data_path_available\": false,\n";
    json << "    \"read_write_direct_data_path_available\": false,\n";
    json << "    \"direct_data_path_available\": false\n";
#endif
    json << "  },\n";
    json << "  \"perftest\": {\n";
    for (std::size_t i = 0; i < perftest_tools.size(); ++i) {
      json << tool_field(perftest_tools[i], opt.perftest_bin_dir);
      json << (i + 1 == perftest_tools.size() ? "\n" : ",\n");
    }
    json << "  },\n";
    json << "  \"command_line\": \"" << rdma_bench::json_escape(cmd)
         << "\"\n";
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
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
