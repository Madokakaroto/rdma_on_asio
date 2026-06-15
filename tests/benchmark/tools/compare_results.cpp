#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "rdma_bench_common.hpp"

struct row {
  std::string file;
  std::string baseline;
  std::string backend;
  std::string topology;
  std::string operation;
  std::string metric;
  std::string mode;
  std::uint64_t message_size = 0;
  std::uint64_t queue_depth = 0;
  std::optional<double> gbit_s;
  std::optional<double> latency_p50_us;
  std::optional<double> latency_p99_us;
  std::string skip_reason;
  std::string first_error;
};

std::string value_or(std::optional<std::string> v, std::string fallback = "") {
  return v ? *v : fallback;
}

std::string fmt(std::optional<double> v) {
  if (!v) return "";
  std::ostringstream os;
  os << std::fixed << std::setprecision(3) << *v;
  return os.str();
}

row load_row(std::filesystem::path const& path) {
  auto json = rdma_bench::read_text_file(path);
  row r;
  r.file = path.string();
  r.baseline = value_or(rdma_bench::json_string_value(json, "baseline"));
  r.backend = value_or(rdma_bench::json_string_value(json, "backend"));
  r.topology = value_or(rdma_bench::json_string_value(json, "topology"));
  r.operation = value_or(rdma_bench::json_string_value(json, "operation"));
  r.metric = value_or(rdma_bench::json_string_value(json, "metric"));
  r.mode = value_or(rdma_bench::json_string_value(json, "completion_mode"));
  r.message_size =
      rdma_bench::json_u64_value(json, "message_size_bytes").value_or(0);
  r.queue_depth = rdma_bench::json_u64_value(json, "queue_depth").value_or(0);
  r.gbit_s = rdma_bench::json_double_value(json, "throughput_gbit_s");
  r.latency_p50_us = rdma_bench::json_double_value(json, "latency_p50_us");
  r.latency_p99_us = rdma_bench::json_double_value(json, "latency_p99_us");
  r.skip_reason = value_or(rdma_bench::json_string_value(json, "skip_reason"));
  r.first_error = value_or(rdma_bench::json_string_value(json, "first_error"));
  return r;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <result.json> [result.json...]\n";
    return 1;
  }

  try {
    std::vector<row> rows;
    for (int i = 1; i < argc; ++i) {
      rows.push_back(load_row(argv[i]));
    }

    std::cout << "| file | baseline | backend | topology | operation | metric | "
                 "mode | size | qd | Gbit/s | p50 us | p99 us | note |\n";
    std::cout << "|---|---|---|---|---|---|---|---:|---:|---:|---:|---:|---|\n";
    for (auto const& r : rows) {
      std::string note = !r.first_error.empty() ? r.first_error : r.skip_reason;
      std::cout << "| " << rdma_bench::json_escape(r.file)
                << " | " << r.baseline
                << " | " << r.backend
                << " | " << r.topology
                << " | " << r.operation
                << " | " << r.metric
                << " | " << r.mode
                << " | " << r.message_size
                << " | " << r.queue_depth
                << " | " << fmt(r.gbit_s)
                << " | " << fmt(r.latency_p50_us)
                << " | " << fmt(r.latency_p99_us)
                << " | " << rdma_bench::json_escape(note) << " |\n";
    }
    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
