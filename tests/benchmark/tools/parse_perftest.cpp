#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "rdma_bench_common.hpp"

namespace fs = std::filesystem;

std::vector<double> numbers_from_line(std::string const& line) {
  static std::regex number_re(R"((-?\d+(?:\.\d+)?))");
  std::vector<double> out;
  for (std::sregex_iterator it(line.begin(), line.end(), number_re), end;
       it != end; ++it) {
    out.push_back(std::stod((*it)[1].str()));
  }
  return out;
}

std::vector<double> last_numeric_row(std::string const& text) {
  std::istringstream in(text);
  std::string line;
  std::vector<double> last;
  while (std::getline(in, line)) {
    auto nums = numbers_from_line(line);
    if (nums.size() >= 3) last = std::move(nums);
  }
  return last;
}

int main(int argc, char* argv[]) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv, false);
    auto cmd = rdma_bench::command_line(argc, argv);
    opt.baseline = "perftest";
    auto r = rdma_bench::make_base_result(opt, cmd);
    r.scenario_name = "perftest_parsed";

    if (opt.raw_stdout.empty()) {
      r.errors = 1;
      r.first_error = "--raw-stdout is required";
      r.exit_code = 1;
      rdma_bench::write_result(r, opt.json_out);
      return 1;
    }

    auto raw = rdma_bench::read_text_file(opt.raw_stdout);
    auto nums = last_numeric_row(raw);
    if (nums.empty()) {
      r.errors = 1;
      r.first_error = "no numeric perftest result row found";
      r.exit_code = 1;
      rdma_bench::write_result(r, opt.json_out);
      return 1;
    }

    if (opt.metric == rdma_bench::metric_kind::bandwidth) {
      // perftest bandwidth rows vary by version. Keep the first parser modest:
      // use the final numeric column as message rate if present and the
      // penultimate column as average bandwidth in MB/sec-like units.
      if (nums.size() >= 2) {
        double mb_per_sec = nums[nums.size() - 2];
        r.throughput_mib_s = mb_per_sec;
        r.throughput_gbit_s = mb_per_sec * 8.0 / 1000.0;
      }
      if (!nums.empty()) r.message_rate_s = nums.back();
      r.completed_count = opt.iterations;
      r.payload_bytes = r.completed_count * opt.message_size;
    } else {
      // Common ib_*_lat rows include min/median/max or enough ordered columns
      // for a first-pass min/p50/max mapping.
      if (nums.size() >= 3) {
        r.latency_min_us = nums[nums.size() - 3];
        r.latency_p50_us = nums[nums.size() - 2];
        r.latency_avg_us = nums[nums.size() - 2];
        r.latency_max_us = nums[nums.size() - 1];
        r.latency_p90_us = std::nullopt;
        r.latency_p99_us = std::nullopt;
        r.latency_sample_method = "perftest_summary";
      }
      r.completed_count = opt.iterations;
    }
    r.validation_passed = true;
    rdma_bench::write_result(r, opt.json_out);
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
