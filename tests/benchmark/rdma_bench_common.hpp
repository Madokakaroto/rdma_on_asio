#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/resource.h>
#  include <sys/time.h>
#endif

#include "asio_perftest_clock.hpp"

namespace rdma_bench {

enum class metric_kind {
  bandwidth,
  latency,
};

enum class operation_kind {
  send_recv,
  write,
  read,
};

struct options {
  bool server = false;
  bool client = false;
  bool single_process = false;
  bool dry_run = false;
  bool execute = false;
  bool strict = false;
  std::string local_addr;
  std::string peer_addr;
  std::uint16_t port = 5000;
  std::string backend = "auto";
  std::string baseline = "rdma_on_asio";
  operation_kind operation = operation_kind::send_recv;
  metric_kind metric = metric_kind::bandwidth;
  std::string mode = "event";
  std::string token_type = "callback";
  std::string topology = "single_host_same_process";
  std::string scenario_name = "manual";
  std::string scenario_path;
  std::size_t message_size = 4096;
  std::uint64_t iterations = 1000;
  double duration_sec = 0.0;
  std::uint32_t queue_depth = 16;
  std::uint32_t qps = 1;
  std::uint32_t threads = 1;
  std::uint32_t warmup_iterations = 0;
  std::uint32_t inline_size = 0;
  std::uint32_t cq_mod = 1;
  std::uint32_t post_list = 1;
  std::uint32_t recv_post_list = 1;
  std::uint32_t signaled_every = 1;
  std::uint32_t timeout_sec = 30;
  // perftest-parity flags accepted by the CLI. margin/no_peak/report_gbits are
  // recorded here so the same command line drives both tools (Stage 9a aliases);
  // they are acted on in Stage 9b/10, not yet.
  double margin_sec = 0.0;
  bool no_peak = false;
  bool report_gbits = false;
  std::string perftest_bin_dir;
  std::string perftest_source_dir;
  std::string output_dir;
  std::string json_out;
  std::string raw_stdout;
  std::string raw_stderr;
};

inline std::string json_escape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char ch : input) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += ch; break;
    }
  }
  return out;
}

inline std::uint64_t parse_u64(std::string_view value,
                               std::string_view name) {
  std::string text(value);
  std::size_t pos = 0;
  auto parsed = std::stoull(text, &pos);
  if (pos != text.size()) {
    throw std::invalid_argument("invalid integer for " + std::string(name));
  }
  return parsed;
}

inline std::string take_value(int& i, int argc, char* argv[],
                              std::string_view name) {
  if (i + 1 >= argc) {
    throw std::invalid_argument("missing value for " + std::string(name));
  }
  return argv[++i];
}

inline bool split_equals(std::string_view arg, std::string& key,
                         std::string& value) {
  auto const pos = arg.find('=');
  if (pos == std::string_view::npos) return false;
  key.assign(arg.substr(0, pos));
  value.assign(arg.substr(pos + 1));
  return true;
}

inline operation_kind parse_operation(std::string const& operation) {
  if (operation == "send_recv") return operation_kind::send_recv;
  if (operation == "write") return operation_kind::write;
  if (operation == "read") return operation_kind::read;
  throw std::invalid_argument("unsupported --operation: " + operation);
}

inline char const* operation_name(operation_kind operation) {
  switch (operation) {
    case operation_kind::send_recv: return "send_recv";
    case operation_kind::write: return "write";
    case operation_kind::read: return "read";
  }
  return "unknown";
}

inline char const* metric_name(metric_kind metric) {
  return metric == metric_kind::bandwidth ? "bandwidth" : "latency";
}

inline void validate_and_normalize_options(options& opt, bool require_role) {
  if (require_role && !opt.single_process && !opt.server && !opt.client) {
    throw std::invalid_argument("choose --single-process, --server, or --client");
  }
  if (require_role && opt.client && opt.peer_addr.empty()) {
    throw std::invalid_argument("--client requires HOST or --peer-addr");
  }
  if (require_role && opt.single_process && opt.local_addr.empty()) {
    throw std::invalid_argument("--single-process requires --local-addr");
  }
  if (opt.message_size == 0) {
    throw std::invalid_argument("--message-size must be greater than zero");
  }
  if (opt.iterations == 0) {
    throw std::invalid_argument("--iterations must be greater than zero");
  }
  if (opt.queue_depth == 0) opt.queue_depth = 1;
  // Latency is a one-outstanding ping-pong (perftest does the same). Forcing
  // queue_depth=1 here also keeps the write-latency server (which validates
  // queue_depth buffer slots) consistent with the single slot the client writes.
  if (opt.metric == metric_kind::latency) opt.queue_depth = 1;
  if (opt.mode != "event" && opt.mode != "poll") {
    throw std::invalid_argument("unsupported --mode: " + opt.mode);
  }
  if (opt.token_type != "callback" && opt.token_type != "use_future" &&
      opt.token_type != "use_awaitable") {
    throw std::invalid_argument("unsupported --token-type: " + opt.token_type);
  }
}

inline options parse_options(int argc, char* argv[], bool require_role = true) {
  options opt;
  for (int i = 1; i < argc; ++i) {
    std::string key;
    std::string value;
    std::string_view arg(argv[i]);
    bool has_inline_value = split_equals(arg, key, value);
    std::string_view name = has_inline_value ? std::string_view(key) : arg;
    auto need_value = [&]() -> std::string {
      return has_inline_value ? value : take_value(i, argc, argv, name);
    };

    if (name == "--server") {
      opt.server = true;
    } else if (name == "--client") {
      opt.client = true;
      if (!has_inline_value && i + 1 < argc && argv[i + 1][0] != '-') {
        opt.peer_addr = argv[++i];
      } else if (has_inline_value) {
        opt.peer_addr = value;
      }
    } else if (name == "--single-process") {
      opt.single_process = true;
    } else if (name == "--local-addr") {
      opt.local_addr = need_value();
    } else if (name == "--peer-addr") {
      opt.peer_addr = need_value();
    } else if (name == "--port") {
      opt.port = static_cast<std::uint16_t>(parse_u64(need_value(), name));
    } else if (name == "--backend") {
      opt.backend = need_value();
    } else if (name == "--baseline") {
      opt.baseline = need_value();
    } else if (name == "--operation") {
      opt.operation = parse_operation(need_value());
    } else if (name == "--metric") {
      auto metric = need_value();
      if (metric == "bandwidth") {
        opt.metric = metric_kind::bandwidth;
      } else if (metric == "latency") {
        opt.metric = metric_kind::latency;
      } else {
        throw std::invalid_argument("unsupported --metric: " + metric);
      }
    } else if (name == "--mode") {
      opt.mode = need_value();
    } else if (name == "--token-type") {
      opt.token_type = need_value();
    } else if (name == "--topology") {
      opt.topology = need_value();
    } else if (name == "--scenario-name") {
      opt.scenario_name = need_value();
    } else if (name == "--scenario") {
      opt.scenario_path = need_value();
    } else if (name == "--message-size") {
      opt.message_size = static_cast<std::size_t>(parse_u64(need_value(), name));
    } else if (name == "--iterations") {
      opt.iterations = parse_u64(need_value(), name);
    } else if (name == "--duration-sec") {
      opt.duration_sec = std::stod(need_value());
    } else if (name == "--queue-depth") {
      opt.queue_depth =
          static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--qps") {
      opt.qps = static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--threads") {
      opt.threads = static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--warmup-iterations") {
      opt.warmup_iterations =
          static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--inline-size") {
      opt.inline_size =
          static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--cq-mod") {
      opt.cq_mod = static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--post-list") {
      opt.post_list =
          static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--recv-post-list") {
      opt.recv_post_list =
          static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--signaled-every") {
      opt.signaled_every =
          static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--timeout-sec") {
      opt.timeout_sec =
          static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--perftest-bin-dir") {
      opt.perftest_bin_dir = need_value();
    } else if (name == "--perftest-source-dir") {
      opt.perftest_source_dir = need_value();
    } else if (name == "--output-dir") {
      opt.output_dir = need_value();
    } else if (name == "--json-out") {
      opt.json_out = need_value();
    } else if (name == "--raw-stdout") {
      opt.raw_stdout = need_value();
    } else if (name == "--raw-stderr") {
      opt.raw_stderr = need_value();
    } else if (name == "--dry-run") {
      opt.dry_run = true;
    } else if (name == "--execute") {
      opt.execute = true;
    } else if (name == "--strict") {
      opt.strict = true;
    } else if (name == "--help" || name == "-h") {
      throw std::runtime_error("help");
    }
    // --- perftest flag aliases (Stage 9a): same command line drives both tools.
    else if (name == "--size" || name == "-s") {
      opt.message_size = static_cast<std::size_t>(parse_u64(need_value(), name));
    } else if (name == "--iters" || name == "-n") {
      opt.iterations = parse_u64(need_value(), name);
    } else if (name == "--tx-depth" || name == "-t" || name == "--rx-depth") {
      opt.queue_depth =
          static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--duration" || name == "-D") {
      opt.duration_sec = std::stod(need_value());
    } else if (name == "--margin") {
      opt.margin_sec = std::stod(need_value());
    } else if (name == "--qp") {
      opt.qps = static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--inline_size") {  // perftest underscore spelling
      opt.inline_size =
          static_cast<std::uint32_t>(parse_u64(need_value(), name));
    } else if (name == "--connection" || name == "-c") {
      auto conn = need_value();
      if (conn != "RC") {
        throw std::invalid_argument(
            "asio_perftest supports --connection RC only (got " + conn + ")");
      }
    } else if (name == "--rdma_cm" || name == "-R") {
      // The library control plane is always rdma_cm; accept for CLI parity.
    } else if (name == "--events") {
      opt.mode = "event";  // perftest event/completion-channel mode
    } else if (name == "--no-peak" || name == "-N" || name == "--noPeak") {
      opt.no_peak = true;  // accepted now; peak handling lands in Stage 10
    } else if (name == "--report-gbits" || name == "--report_gbits") {
      opt.report_gbits = true;
    } else {
      throw std::invalid_argument("unknown option: " + std::string(name));
    }
  }

  validate_and_normalize_options(opt, require_role);
  return opt;
}

struct result {
  std::string schema_version = "1";
  std::string scenario_name = "send_recv";
  std::string backend = "auto";
  std::string baseline = "rdma_on_asio";
  std::string topology;
  std::string operation = "send_recv";
  std::string metric;
  std::string completion_mode;
  std::string token_type;
  std::size_t message_size_bytes = 0;
  std::uint64_t queue_depth = 0;
  std::uint64_t qps = 1;
  std::uint64_t threads = 1;
  // perftest parity knobs in effect for this run. This iteration they are clamped
  // to the supported defaults (non-default values are rejected by the
  // not_implemented gate); recorded so a comparison row is self-describing.
  std::uint32_t cq_mod = 1;
  std::uint32_t inline_size = 0;
  std::uint32_t post_list = 1;
  std::uint32_t recv_post_list = 1;
  std::uint64_t iterations = 0;
  double duration_sec = 0.0;
  std::uint64_t warmup_iterations = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t posted_count = 0;
  std::uint64_t completed_count = 0;
  double throughput_mib_s = 0.0;
  double throughput_gbit_s = 0.0;
  double message_rate_s = 0.0;
  double bw_peak_gbit_s = 0.0;
  std::optional<double> latency_avg_us;
  std::optional<double> latency_min_us;
  std::optional<double> latency_p50_us;       // t_typical (median), perftest naming
  std::optional<double> latency_p90_us;
  std::optional<double> latency_p99_us;
  std::optional<double> latency_p999_us;      // 99.9 percentile (perftest)
  std::optional<double> latency_stdev_us;     // perftest t_stdev
  std::optional<double> latency_max_us;
  // Latency distribution: fixed log-ish us buckets (le = inclusive upper edge,
  // last bucket = overflow with le = +inf). Filled by fill_latency_cycles.
  std::vector<std::pair<double, std::uint64_t>> latency_histogram;
  std::string latency_sample_method = "null";
  std::uint64_t clock_overhead_ns = 0;
  std::optional<double> cpu_cycles_per_op;
  std::optional<double> cpu_util_percent;
  std::optional<std::uint64_t> context_switches;
  std::uint64_t rnr_retry_events = 0;
  std::optional<bool> validation_passed;
  std::uint64_t errors = 0;
  std::string first_error;
  int exit_code = 0;
  std::string skip_reason;
  std::string missing_capability;
  std::string command_line;
  std::string environment_json = "{}";
};

inline std::string compiler_name() {
#if defined(_MSC_VER)
  return "msvc";
#elif defined(__clang__)
  return "clang";
#elif defined(__GNUC__)
  return "gcc";
#else
  return "unknown";
#endif
}

inline std::string platform_name() {
#if defined(_WIN32)
  return "windows";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

struct cpu_snapshot {
  double process_cpu_sec = 0.0;
  std::uint64_t ctx_switches = 0;  // voluntary + involuntary (Linux); 0 on Windows
};

#if defined(_WIN32)
inline double filetime_to_seconds(FILETIME const& ft) {
  ULARGE_INTEGER value{};
  value.LowPart = ft.dwLowDateTime;
  value.HighPart = ft.dwHighDateTime;
  return static_cast<double>(value.QuadPart) / 10000000.0;
}
#endif

inline cpu_snapshot take_cpu_snapshot() {
  cpu_snapshot out;
#if defined(_WIN32)
  FILETIME create_time{}, exit_time{}, kernel_time{}, user_time{};
  if (::GetProcessTimes(::GetCurrentProcess(), &create_time, &exit_time,
                        &kernel_time, &user_time)) {
    out.process_cpu_sec =
        filetime_to_seconds(kernel_time) + filetime_to_seconds(user_time);
  }
#else
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) == 0) {
    out.process_cpu_sec =
        static_cast<double>(usage.ru_utime.tv_sec) +
        static_cast<double>(usage.ru_utime.tv_usec) / 1000000.0 +
        static_cast<double>(usage.ru_stime.tv_sec) +
        static_cast<double>(usage.ru_stime.tv_usec) / 1000000.0;
    out.ctx_switches = static_cast<std::uint64_t>(usage.ru_nvcsw) +
                       static_cast<std::uint64_t>(usage.ru_nivcsw);
  }
#endif
  return out;
}

inline unsigned cpu_count() {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  ::GetSystemInfo(&info);
  return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
#else
  auto n = std::thread::hardware_concurrency();
  return n ? n : 1;
#endif
}

inline void fill_cpu_metrics(result& r, cpu_snapshot begin,
                             cpu_snapshot end) {
  if (r.duration_sec <= 0.0) return;
  double const cpu_delta = end.process_cpu_sec - begin.process_cpu_sec;
  if (cpu_delta < 0.0) return;
  r.cpu_util_percent = (cpu_delta / r.duration_sec) * 100.0 /
                       static_cast<double>(cpu_count());
#if !defined(_WIN32)
  // Context switches over the measured window (getrusage voluntary +
  // involuntary) -- a scheduling-pressure signal alongside cpu_util.
  if (end.ctx_switches >= begin.ctx_switches) {
    r.context_switches = end.ctx_switches - begin.ctx_switches;
  }
#endif
  if (r.completed_count != 0) {
    // Abstraction-cost metric (no perftest counterpart): process CPU time
    // (user+sys) converted to cycles via the measured cpu_mhz, per completed op.
    // cpu_mhz() is the same calibrated value the cycle-counter BW/latency paths
    // use, so cycles/op is consistent with those numbers.
    r.cpu_cycles_per_op =
        cpu_delta * cpu_mhz() * 1.0e6 / static_cast<double>(r.completed_count);
  }
}

// ---- Stage 9b: warmup / duration / margin window engine --------------------
// One instance per measured stream (per client/server role). Drives the
// warmup -> timed-window lifecycle by op SEQUENCE (the first `warmup_iterations`
// ops, in post/completion order, are excluded) and stops by op-count (iters
// mode, duration_sec==0) or wall deadline (duration mode). Sample recording is
// keyed on sequence > warmup so a BW pipeline's in-flight ops that straddle the
// warmup->window boundary still pair tposted[i] with tcompleted[i] (RC is FIFO,
// so the k-th post and the k-th completion are the same op). Margin trimming is
// post-hoc over the recorded arrays and applies only in duration mode.
class window_controller {
 public:
  using clock_type = std::chrono::steady_clock;
  window_controller(options const& opt, std::size_t reserve_hint)
      : warmup_(opt.warmup_iterations),
        iters_(opt.iterations),
        duration_sec_(opt.duration_sec),
        margin_sec_(opt.margin_sec) {
    if (reserve_hint) {
      tposted_.reserve(reserve_hint);
      tcompleted_.reserve(reserve_hint);
      deltas_.reserve(reserve_hint);
    }
  }

  bool duration_mode() const { return duration_sec_ > 0.0; }

  // Record an issued op (post order). Warmup posts (seq <= warmup) excluded.
  void note_post() {
    ++posted_seq_;
    maybe_open();
    if (posted_seq_ > warmup_) tposted_.push_back(get_cycles());
  }
  // Record a BW completion (completion order). Warmup completions excluded.
  void note_complete_bw() {
    ++completed_seq_;
    maybe_open();
    if (completed_seq_ > warmup_) {
      tcompleted_.push_back(get_cycles());
      ++in_window_;
    }
  }
  // Record a latency sample (one-outstanding ping-pong). Warmup excluded.
  void note_complete_lat(cycles_t delta) {
    ++completed_seq_;
    maybe_open();
    if (completed_seq_ > warmup_) {
      deltas_.push_back(delta);
      ++in_window_;
    }
  }

  // True exactly once, at the warmup->window transition: the caller snapshots
  // CPU and sends the 'W' barrier byte here.
  bool take_opened() {
    if (!opened_pending_) return false;
    opened_pending_ = false;
    return true;
  }
  bool opened() const { return opened_; }

  // Stop the timed window? iters: in-window completions reached the target.
  // duration: wall clock past the deadline.
  bool window_done() const {
    if (!opened_) return false;
    if (duration_mode()) return clock_type::now() >= win_deadline_;
    return in_window_ >= iters_;
  }
  // Keep posting (for in-loop reposts; priming up to queue depth is the caller's
  // job)? iters: up to warmup+iters total posts. duration: until the deadline.
  bool should_post() const {
    if (duration_mode()) return !opened_ || clock_type::now() < win_deadline_;
    return posted_seq_ < warmup_ + iters_;
  }

  std::uint64_t in_window_count() const { return in_window_; }
  std::uint64_t warmup_done_count() const {
    return completed_seq_ < warmup_ ? completed_seq_ : warmup_;
  }
  clock_type::time_point window_begin_wall() const { return win_begin_wall_; }

  // Margin-trimmed views for finish_*. Margin applied ONLY in duration mode.
  void bw_arrays(std::vector<cycles_t>& tp, std::vector<cycles_t>& tc) const;
  std::vector<cycles_t> lat_deltas() const;

 private:
  void maybe_open() {
    if (opened_ || completed_seq_ < warmup_) return;
    opened_ = true;
    opened_pending_ = true;
    win_begin_cyc_ = get_cycles();
    win_begin_wall_ = clock_type::now();
    win_deadline_ = win_begin_wall_ +
        std::chrono::duration_cast<clock_type::duration>(
            std::chrono::duration<double>(duration_sec_));
  }

  std::uint32_t warmup_;
  std::uint64_t iters_;
  double duration_sec_;
  double margin_sec_;
  std::uint64_t posted_seq_ = 0;
  std::uint64_t completed_seq_ = 0;
  std::uint64_t in_window_ = 0;
  bool opened_ = false;
  bool opened_pending_ = false;
  cycles_t win_begin_cyc_ = 0;
  clock_type::time_point win_begin_wall_{};
  clock_type::time_point win_deadline_{};
  std::vector<cycles_t> tposted_;
  std::vector<cycles_t> tcompleted_;
  std::vector<cycles_t> deltas_;
};

inline void window_controller::bw_arrays(std::vector<cycles_t>& tp,
                                         std::vector<cycles_t>& tc) const {
  std::size_t const n = std::min(tposted_.size(), tcompleted_.size());
  if (n == 0 || margin_sec_ <= 0.0 || !duration_mode()) {
    tp.assign(tposted_.begin(), tposted_.begin() + n);
    tc.assign(tcompleted_.begin(), tcompleted_.begin() + n);
    return;
  }
  cycles_t const margin_cyc = static_cast<cycles_t>(margin_sec_ * cpu_mhz() * 1e6);
  cycles_t const lo = win_begin_cyc_ + margin_cyc;
  cycles_t const hi = tcompleted_[n - 1] - margin_cyc;
  std::size_t a = 0, b = n;
  while (a < n && tposted_[a] < lo) ++a;
  while (b > a && tcompleted_[b - 1] > hi) --b;
  tp.assign(tposted_.begin() + a, tposted_.begin() + b);
  tc.assign(tcompleted_.begin() + a, tcompleted_.begin() + b);
}

inline std::vector<cycles_t> window_controller::lat_deltas() const {
  if (deltas_.empty() || margin_sec_ <= 0.0 || !duration_mode()) return deltas_;
  cycles_t const margin_cyc = static_cast<cycles_t>(margin_sec_ * cpu_mhz() * 1e6);
  std::size_t a = 0, b = deltas_.size();
  cycles_t acc = 0;
  while (a < b && acc < margin_cyc) { acc += deltas_[a]; ++a; }
  acc = 0;
  while (b > a && acc < margin_cyc) { acc += deltas_[b - 1]; --b; }
  return std::vector<cycles_t>(deltas_.begin() + a, deltas_.begin() + b);
}

// Single place that writes the in-window completion count + actual warmup count
// from a controller, so all roles report consistently. completed_count excludes
// warmup; the plan's invariant (measured == completed_count) holds by construction.
inline void finalize_counts(result& r, window_controller const& win) {
  r.completed_count = win.in_window_count();
  r.warmup_iterations = win.warmup_done_count();
}

inline std::string collect_environment_json() {
  std::ostringstream os;
  os << "{";
#ifdef RDMA_BENCH_BACKEND
  os << "\"backend_build\":\"" << json_escape(RDMA_BENCH_BACKEND) << "\",";
#else
  os << "\"backend_build\":\"unknown\",";
#endif
#ifdef RDMA_BENCH_BUILD_TYPE
  os << "\"build_type\":\"" << json_escape(RDMA_BENCH_BUILD_TYPE) << "\",";
#else
  os << "\"build_type\":\"unknown\",";
#endif
  os << "\"compiler\":\"" << compiler_name() << "\",";
  os << "\"platform\":\"" << platform_name() << "\",";
  os << "\"cpu_count\":" << cpu_count() << ",";
#if defined(_MSVC_LANG)
  os << "\"cpp_standard\":" << _MSVC_LANG;
#else
  os << "\"cpp_standard\":" << __cplusplus;
#endif
  os << "}";
  return os.str();
}

inline std::uint64_t calibrate_clock_overhead_ns() {
  using clock = std::chrono::steady_clock;
  constexpr int samples = 1000;
  auto best = clock::duration::max();
  for (int i = 0; i < samples; ++i) {
    auto a = clock::now();
    auto b = clock::now();
    best = std::min(best, b - a);
  }
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(best).count());
}

inline result make_base_result(options const& opt, std::string command_line) {
  result r;
  r.scenario_name = opt.scenario_name;
  r.backend = opt.backend;
  r.baseline = opt.baseline;
  r.topology = opt.topology;
  r.operation = operation_name(opt.operation);
  r.metric = metric_name(opt.metric);
  r.completion_mode = opt.mode;
  r.token_type = opt.token_type;
  r.message_size_bytes = opt.message_size;
  r.queue_depth = opt.queue_depth;
  r.qps = opt.qps;
  r.threads = opt.threads;
  r.cq_mod = opt.cq_mod;
  r.inline_size = opt.inline_size;
  r.post_list = opt.post_list;
  r.recv_post_list = opt.recv_post_list;
  r.iterations = opt.iterations;
  r.duration_sec = opt.duration_sec;
  r.warmup_iterations = opt.warmup_iterations;
  r.clock_overhead_ns = calibrate_clock_overhead_ns();
  r.command_line = std::move(command_line);
  r.environment_json = collect_environment_json();
  return r;
}

template <typename Clock>
inline void finish_throughput(result& r,
                              std::chrono::time_point<Clock> begin,
                              std::chrono::time_point<Clock> end) {
  auto elapsed = std::chrono::duration<double>(end - begin).count();
  if (elapsed <= 0.0) elapsed = 0.000001;
  r.duration_sec = elapsed;
  r.payload_bytes = r.completed_count * r.message_size_bytes;
  r.throughput_mib_s =
      static_cast<double>(r.payload_bytes) / (1024.0 * 1024.0) / elapsed;
  r.throughput_gbit_s =
      static_cast<double>(r.payload_bytes) * 8.0 / 1000000000.0 / elapsed;
  r.message_rate_s = static_cast<double>(r.completed_count) / elapsed;
}

inline result make_skip_result(options const& opt, std::string command_line,
                               std::string reason,
                               std::string missing_capability = {}) {
  auto r = make_base_result(opt, std::move(command_line));
  r.skip_reason = std::move(reason);
  r.missing_capability = std::move(missing_capability);
  return r;
}

// Perftest knobs that need rdma-on-asio interface/impl work that is deliberately
// out of scope this iteration (see plan Stage 11/12). asio_perftest accepts the
// flags for command-line parity with perftest, but cannot honor a non-default
// value through the current public API, so it returns a not-implemented skip
// rather than silently producing a number that does not reflect the requested
// per-message work. Returns the first offending reason, or empty if all knobs are
// at their supported defaults. (--connection non-RC is rejected earlier at parse.)
inline std::string not_implemented_reason(options const& opt) {
  if (opt.inline_size > 0) {
    return "inline_size requires QP max_inline_data + IBV_SEND_INLINE, not "
           "implemented this iteration";
  }
  if (opt.cq_mod > 1 || opt.signaled_every > 1) {
    return "cq-mod / signaled-every selective signaling not implemented "
           "(RDMA-on-Asio signals every WR)";
  }
  if (opt.post_list > 1) {
    return "post_list WR batching not implemented (one WR per post)";
  }
  if (opt.recv_post_list > 1) {
    return "recv_post_list WR batching not implemented (one WR per post)";
  }
  if (opt.qps > 1) {
    return "multi-QP (--qp / --qps > 1) needs N QPs over one connection, which "
           "the connector API does not support (one cm_id == one QP); not "
           "implemented this iteration";
  }
  return {};
}

inline double percentile(std::vector<double> samples, double p) {
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  double const idx = (p / 100.0) * static_cast<double>(samples.size() - 1);
  auto const lo = static_cast<std::size_t>(idx);
  auto const hi = std::min(lo + 1, samples.size() - 1);
  double const frac = idx - static_cast<double>(lo);
  return samples[lo] + (samples[hi] - samples[lo]) * frac;
}

inline void fill_latency(result& r, std::vector<double> const& samples) {
  if (samples.empty()) return;
  double sum = 0.0;
  for (double v : samples) sum += v;
  r.latency_avg_us = sum / static_cast<double>(samples.size());
  r.latency_min_us = *std::min_element(samples.begin(), samples.end());
  r.latency_p50_us = percentile(samples, 50.0);
  r.latency_p90_us = percentile(samples, 90.0);
  r.latency_p99_us = percentile(samples, 99.0);
  r.latency_max_us = *std::max_element(samples.begin(), samples.end());
  r.latency_sample_method = "full_array";
}

// perftest-faithful latency stats (mirrors print_report_lat) from per-iteration
// cycle deltas: sort, drop the top LAT_MEASURE_TAIL=2 samples, then
// t_min/t_max/t_typical(median)/t_avg/t_stdev/99%/99.9%, all divided by
// cpu_mhz * rtt_factor. rtt_factor = 2 for send/recv & write (report half the
// round trip), 1 for read/atomic (the one-sided completion is already a full
// round trip). NOTE: perftest computes stdev with an int-truncated deviation
// (get_clock: `int temp_var`); we keep full double precision instead.
inline void fill_latency_cycles(result& r, std::vector<cycles_t> deltas,
                                int rtt_factor) {
  constexpr std::size_t kTail = 2;  // perftest LAT_MEASURE_TAIL
  if (deltas.size() <= kTail) return;
  std::sort(deltas.begin(), deltas.end());
  std::size_t const n = deltas.size();
  std::size_t const m = n - kTail;  // measure_cnt after dropping the top 2
  double const q = cpu_mhz() * static_cast<double>(rtt_factor);  // cycles->usec
  auto us = [&](std::size_t i) { return static_cast<double>(deltas[i]) / q; };
  double median = ((m - 1) % 2) ? (us(m / 2) + us(m / 2 - 1)) / 2.0 : us(m / 2);
  double sum = 0.0;
  for (std::size_t i = 0; i < m; ++i) sum += us(i);
  double avg = sum / static_cast<double>(m);
  double var = 0.0;
  for (std::size_t i = 0; i < m; ++i) {
    double d = avg - us(i);
    var += d * d;
  }
  auto idx = [&](double frac) {
    auto k = static_cast<std::size_t>(std::ceil(static_cast<double>(m) * frac));
    return std::min(k, n - 1);
  };
  r.latency_min_us = us(0);
  r.latency_max_us = us(m);  // perftest delta[measure_cnt]
  r.latency_p50_us = median;
  r.latency_avg_us = avg;
  r.latency_stdev_us = std::sqrt(var / static_cast<double>(m));
  r.latency_p90_us = us(idx(0.90));
  r.latency_p99_us = us(idx(0.99));
  r.latency_p999_us = us(idx(0.999));
  // Latency distribution over the measured samples [0, m): fixed log-ish us
  // buckets. Each entry is (le_us, count) with le the inclusive upper edge; the
  // final entry uses le = -1 to denote the overflow bucket (> last edge).
  static constexpr double kEdges[] = {0.5, 1,   2,   4,    8,    16,  32,
                                      64,  128, 256, 512,  1024, 4096};
  constexpr std::size_t kN = sizeof(kEdges) / sizeof(kEdges[0]);
  std::vector<std::uint64_t> counts(kN + 1, 0);
  for (std::size_t i = 0; i < m; ++i) {
    double const v = us(i);
    std::size_t b = 0;
    while (b < kN && v > kEdges[b]) ++b;
    ++counts[b];
  }
  r.latency_histogram.clear();
  for (std::size_t b = 0; b < kN; ++b)
    r.latency_histogram.emplace_back(kEdges[b], counts[b]);
  r.latency_histogram.emplace_back(-1.0, counts[kN]);  // overflow (> last edge)
  r.latency_sample_method = "cycle_counter";
}

// perftest-faithful bandwidth (mirrors print_report_bw): average from total
// elapsed cycles; peak from the minimum per-message window over the per-op
// timestamp arrays. tposted[i]/tcompleted[i] are cycle stamps at post/completion
// (in post and completion order). post_list/cq_mod are 1 in the library today.
inline void finish_bw_cycles(result& r, std::vector<cycles_t> const& tposted,
                             std::vector<cycles_t> const& tcompleted,
                             std::uint32_t post_list, std::uint32_t cq_mod,
                             bool no_peak) {
  std::size_t const n = std::min(tposted.size(), tcompleted.size());
  if (n == 0) return;
  double const cyc_per_sec = cpu_mhz() * 1e6;
  double const total_cycles =
      static_cast<double>(tcompleted[n - 1] - tposted[0]);
  double const seconds = total_cycles > 0 ? total_cycles / cyc_per_sec : 1e-9;
  double const total_bytes =
      static_cast<double>(n) * static_cast<double>(r.message_size_bytes);
  r.duration_sec = seconds;
  r.payload_bytes = static_cast<std::uint64_t>(total_bytes);
  r.throughput_gbit_s = total_bytes / (seconds * 125000000.0);
  r.throughput_mib_s = total_bytes / (seconds * 1048576.0);
  r.message_rate_s = static_cast<double>(n) / seconds;

  // The peak scan below is O(n^2). iters-mode n is bounded by --iters, but
  // duration mode has no a-priori bound (it runs for the whole window), so cap
  // the scan: above the cap report peak == average (the duration-mode
  // convention -- a long window has no meaningful instantaneous peak anyway).
  constexpr std::size_t kPeakScanMax = 20000;
  if (no_peak || n > kPeakScanMax) {
    r.bw_peak_gbit_s = r.throughput_gbit_s;
    return;
  }
  if (post_list == 0) post_list = 1;
  if (cq_mod == 0) cq_mod = 1;
  double opt_delta = static_cast<double>(tcompleted[0] - tposted[0]);
  for (std::size_t i = 0; i < n; i += post_list) {
    std::size_t jstart = ((i + 1 + cq_mod - 1) / cq_mod) * cq_mod - 1;
    for (std::size_t j = jstart; j < n; j += cq_mod) {
      double t = static_cast<double>(tcompleted[j] - tposted[i]) /
                 static_cast<double>(j - i + 1);
      if (t < opt_delta) opt_delta = t;
    }
  }
  double const min_sec_per_msg = opt_delta / cyc_per_sec;
  r.bw_peak_gbit_s = static_cast<double>(r.message_size_bytes) /
                     (min_sec_per_msg * 125000000.0);
}

inline std::string nullable_bool(std::optional<bool> value) {
  if (!value) return "null";
  return *value ? "true" : "false";
}

inline std::string to_json(result const& r) {
  auto opt_double = [](std::optional<double> value) {
    if (!value) return std::string("null");
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << *value;
    return os.str();
  };
  auto opt_u64 = [](std::optional<std::uint64_t> value) {
    if (!value) return std::string("null");
    return std::to_string(*value);
  };
  auto text_or_null = [](std::string const& value) {
    if (value.empty()) return std::string("null");
    return std::string("\"") + json_escape(value) + "\"";
  };

  std::ostringstream os;
  os << "{\n";
  os << "  \"schema_version\": \"" << json_escape(r.schema_version) << "\",\n";
  os << "  \"scenario_name\": \"" << json_escape(r.scenario_name) << "\",\n";
  os << "  \"backend\": \"" << json_escape(r.backend) << "\",\n";
  os << "  \"baseline\": \"" << json_escape(r.baseline) << "\",\n";
  os << "  \"topology\": \"" << json_escape(r.topology) << "\",\n";
  os << "  \"operation\": \"" << json_escape(r.operation) << "\",\n";
  os << "  \"metric\": \"" << json_escape(r.metric) << "\",\n";
  os << "  \"completion_mode\": \"" << json_escape(r.completion_mode) << "\",\n";
  os << "  \"token_type\": \"" << json_escape(r.token_type) << "\",\n";
  os << "  \"message_size_bytes\": " << r.message_size_bytes << ",\n";
  os << "  \"queue_depth\": " << r.queue_depth << ",\n";
  os << "  \"qps\": " << r.qps << ",\n";
  os << "  \"threads\": " << r.threads << ",\n";
  os << "  \"cq_mod\": " << r.cq_mod << ",\n";
  os << "  \"inline_size\": " << r.inline_size << ",\n";
  os << "  \"post_list\": " << r.post_list << ",\n";
  os << "  \"recv_post_list\": " << r.recv_post_list << ",\n";
  os << "  \"iterations\": " << r.iterations << ",\n";
  os << "  \"duration_sec\": " << std::fixed << std::setprecision(6)
     << r.duration_sec << ",\n";
  os << "  \"warmup_iterations\": " << r.warmup_iterations << ",\n";
  os << "  \"payload_bytes\": " << r.payload_bytes << ",\n";
  os << "  \"posted_count\": " << r.posted_count << ",\n";
  os << "  \"completed_count\": " << r.completed_count << ",\n";
  os << "  \"throughput_mib_s\": " << std::fixed << std::setprecision(3)
     << r.throughput_mib_s << ",\n";
  os << "  \"throughput_gbit_s\": " << std::fixed << std::setprecision(3)
     << r.throughput_gbit_s << ",\n";
  os << "  \"message_rate_s\": " << std::fixed << std::setprecision(3)
     << r.message_rate_s << ",\n";
  os << "  \"bw_peak_gbit_s\": " << std::fixed << std::setprecision(3)
     << r.bw_peak_gbit_s << ",\n";
  os << "  \"latency_avg_us\": " << opt_double(r.latency_avg_us) << ",\n";
  os << "  \"latency_min_us\": " << opt_double(r.latency_min_us) << ",\n";
  os << "  \"latency_p50_us\": " << opt_double(r.latency_p50_us) << ",\n";
  os << "  \"latency_p90_us\": " << opt_double(r.latency_p90_us) << ",\n";
  os << "  \"latency_p99_us\": " << opt_double(r.latency_p99_us) << ",\n";
  os << "  \"latency_p999_us\": " << opt_double(r.latency_p999_us) << ",\n";
  os << "  \"latency_stdev_us\": " << opt_double(r.latency_stdev_us) << ",\n";
  os << "  \"latency_max_us\": " << opt_double(r.latency_max_us) << ",\n";
  os << "  \"latency_histogram\": [";
  for (std::size_t i = 0; i < r.latency_histogram.size(); ++i) {
    if (i) os << ", ";
    os << "{\"le_us\": " << std::fixed << std::setprecision(1)
       << r.latency_histogram[i].first
       << ", \"count\": " << r.latency_histogram[i].second << "}";
  }
  os << "],\n";
  os << "  \"latency_sample_method\": "
     << text_or_null(r.latency_sample_method == "null" ? "" :
                                                     r.latency_sample_method)
     << ",\n";
  os << "  \"clock_overhead_ns\": " << r.clock_overhead_ns << ",\n";
  os << "  \"cpu_cycles_per_op\": " << opt_double(r.cpu_cycles_per_op) << ",\n";
  os << "  \"cpu_util_percent\": " << opt_double(r.cpu_util_percent) << ",\n";
  os << "  \"context_switches\": " << opt_u64(r.context_switches) << ",\n";
  os << "  \"rnr_retry_events\": " << r.rnr_retry_events << ",\n";
  os << "  \"validation_passed\": " << nullable_bool(r.validation_passed)
     << ",\n";
  os << "  \"errors\": " << r.errors << ",\n";
  os << "  \"first_error\": " << text_or_null(r.first_error) << ",\n";
  os << "  \"exit_code\": " << r.exit_code << ",\n";
  os << "  \"skip_reason\": " << text_or_null(r.skip_reason) << ",\n";
  os << "  \"missing_capability\": " << text_or_null(r.missing_capability)
     << ",\n";
  os << "  \"command_line\": \"" << json_escape(r.command_line) << "\",\n";
  os << "  \"environment\": " << r.environment_json << "\n";
  os << "}\n";
  return os.str();
}

// Compact, perftest-flavored one-line summary for stdout. Partial until Stage 10
// adds BW peak and the full latency stats (t_typical/stdev/99.9); generic enough
// for skip/error/stress results too.
inline std::string human_summary(result const& r) {
  std::ostringstream os;
  if (!r.skip_reason.empty()) {
    os << "SKIP  " << r.scenario_name << "  (" << r.skip_reason << ")\n";
    return os.str();
  }
  if (r.errors) {
    os << "ERROR " << r.scenario_name << "  " << r.first_error << "\n";
    return os.str();
  }
  os << std::fixed;
  auto v = [](std::optional<double> o) { return o ? *o : 0.0; };
  if (r.metric == "latency" && r.latency_p50_us) {
    // perftest RESULT_FMT_LAT columns: t_min t_max t_typical t_avg t_stdev 99% 99.9%
    os << " " << r.operation << " lat  " << r.backend << "/" << r.completion_mode
       << "  #bytes " << r.message_size_bytes << "  #iters " << r.completed_count
       << std::setprecision(2)
       << "  t_min " << v(r.latency_min_us) << "  t_max " << v(r.latency_max_us)
       << "  t_typical " << *r.latency_p50_us << "  t_avg " << v(r.latency_avg_us)
       << "  t_stdev " << v(r.latency_stdev_us) << "  99% " << v(r.latency_p99_us)
       << "  99.9% " << v(r.latency_p999_us) << " usec\n";
  } else if (r.metric == "bandwidth") {
    // perftest RESULT_FMT_G columns: BW peak / BW average / MsgRate
    os << " " << r.operation << " bw   " << r.backend << "/" << r.completion_mode
       << "  #bytes " << r.message_size_bytes << "  #iters " << r.completed_count
       << std::setprecision(2)
       << "  BW_peak " << r.bw_peak_gbit_s << "  BW_avg " << r.throughput_gbit_s
       << " Gb/sec  MsgRate " << (r.message_rate_s / 1e6) << " Mpps\n";
  } else {
    os << " " << r.scenario_name << "  posted=" << r.posted_count
       << " completed=" << r.completed_count << " errors=" << r.errors << "\n";
  }
  return os.str();
}

// JSON goes to --json-out when given (so compare_results / scenario runners read
// a clean file), and a perftest-style summary goes to stdout. With no --json-out,
// JSON is written to stdout (back-compat for ad-hoc / stress runs).
inline void write_result(result const& r, std::string const& path) {
  auto json = to_json(r);
  if (!path.empty()) {
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      throw std::runtime_error("failed to open json output: " + path);
    }
    out << json;
    std::cout << human_summary(r);
  } else {
    std::cout << json;
  }
}

inline std::string command_line(int argc, char* argv[]) {
  std::ostringstream os;
  for (int i = 0; i < argc; ++i) {
    if (i) os << ' ';
    os << argv[i];
  }
  return os.str();
}

inline std::string read_text_file(std::filesystem::path const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("failed to open file: " + path.string());
  std::ostringstream os;
  os << in.rdbuf();
  return os.str();
}

inline std::optional<std::string> json_string_value(std::string const& json,
                                                    std::string const& key) {
  std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
  std::smatch match;
  if (std::regex_search(json, match, re)) return match[1].str();
  return std::nullopt;
}

inline std::optional<std::uint64_t> json_u64_value(std::string const& json,
                                                   std::string const& key) {
  std::regex re("\"" + key + "\"\\s*:\\s*(\\d+)");
  std::smatch match;
  if (std::regex_search(json, match, re)) {
    return static_cast<std::uint64_t>(std::stoull(match[1].str()));
  }
  return std::nullopt;
}

inline std::optional<double> json_double_value(std::string const& json,
                                               std::string const& key) {
  std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
  std::smatch match;
  if (std::regex_search(json, match, re)) {
    return std::stod(match[1].str());
  }
  return std::nullopt;
}

inline std::string quote_arg(std::string const& value) {
  if (value.find_first_of(" \t\"") == std::string::npos) return value;
  std::string out = "\"";
  for (char ch : value) {
    if (ch == '"') out += '\\';
    out += ch;
  }
  out += '"';
  return out;
}

inline void apply_scenario(options& opt, std::string const& json) {
  if (auto v = json_string_value(json, "name")) opt.scenario_name = *v;
  if (auto v = json_string_value(json, "backend")) opt.backend = *v;
  if (auto v = json_string_value(json, "operation")) {
    opt.operation = parse_operation(*v);
  }
  if (auto v = json_string_value(json, "metric")) {
    if (*v == "bandwidth") opt.metric = metric_kind::bandwidth;
    else if (*v == "latency") opt.metric = metric_kind::latency;
    else throw std::invalid_argument("unsupported scenario metric: " + *v);
  }
  if (auto v = json_string_value(json, "completion_mode")) opt.mode = *v;
  if (auto v = json_string_value(json, "token_type")) opt.token_type = *v;
  if (auto v = json_string_value(json, "topology")) opt.topology = *v;
  if (auto v = json_u64_value(json, "message_size")) {
    opt.message_size = static_cast<std::size_t>(*v);
  }
  if (auto v = json_u64_value(json, "queue_depth")) {
    opt.queue_depth = static_cast<std::uint32_t>(*v);
  }
  if (auto v = json_u64_value(json, "qps")) {
    opt.qps = static_cast<std::uint32_t>(*v);
  }
  if (auto v = json_u64_value(json, "threads")) {
    opt.threads = static_cast<std::uint32_t>(*v);
  }
  if (auto v = json_u64_value(json, "iterations")) opt.iterations = *v;
  if (auto v = json_double_value(json, "duration_sec")) opt.duration_sec = *v;
  if (auto v = json_double_value(json, "margin_sec")) opt.margin_sec = *v;
  if (auto v = json_u64_value(json, "warmup_iterations")) {
    opt.warmup_iterations = static_cast<std::uint32_t>(*v);
  }
  if (auto v = json_u64_value(json, "inline_size")) {
    opt.inline_size = static_cast<std::uint32_t>(*v);
  }
  if (auto v = json_u64_value(json, "cq_mod")) {
    opt.cq_mod = static_cast<std::uint32_t>(*v);
  }
  if (auto v = json_u64_value(json, "post_list")) {
    opt.post_list = static_cast<std::uint32_t>(*v);
  }
  if (auto v = json_u64_value(json, "recv_post_list")) {
    opt.recv_post_list = static_cast<std::uint32_t>(*v);
  }
  if (auto v = json_u64_value(json, "signaled_every")) {
    opt.signaled_every = static_cast<std::uint32_t>(*v);
  }
}

inline options parse_options_with_scenario(int argc, char* argv[],
                                           bool require_role = true) {
  auto opt = parse_options(argc, argv, require_role);
  if (!opt.scenario_path.empty()) {
    apply_scenario(opt, read_text_file(opt.scenario_path));
    validate_and_normalize_options(opt, require_role);
  }
  return opt;
}

inline void print_usage(char const* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " --single-process --local-addr IP [options]\n"
      << "  " << argv0 << " --server [--local-addr IP] [options]\n"
      << "  " << argv0 << " --client HOST [options]\n"
      << "  " << argv0 << " --client --peer-addr HOST [options]\n\n"
      << "Key options:\n"
      << "  --operation send_recv|write|read\n"
      << "  --metric bandwidth|latency\n"
      << "  --mode event|poll\n"
      << "  --token-type callback|use_future|use_awaitable\n"
      << "  --topology single_host_same_process|single_host_two_process|two_host_direct|two_host_switch|multi_host\n"
      << "  --message-size N --iterations N --queue-depth N --port N\n"
      << "  --scenario PATH --json-out PATH\n";
}

}  // namespace rdma_bench
