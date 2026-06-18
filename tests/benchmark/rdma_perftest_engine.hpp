#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "rdma_bench_common.hpp"

namespace rdma_bench {

inline std::uint64_t planned_total_ops(options const& opt) {
  return static_cast<std::uint64_t>(opt.warmup_iterations) + opt.iterations;
}

inline std::size_t prime_count(options const& opt, std::size_t slots) {
  if (slots == 0) {
    throw std::invalid_argument("benchmark window requires at least one slot");
  }
  if (opt.duration_sec > 0.0) return slots;
  return static_cast<std::size_t>(
      (std::min<std::uint64_t>)(slots, planned_total_ops(opt)));
}

struct bandwidth_window_summary {
  std::uint64_t posted = 0;
  std::uint64_t completed = 0;
};

// Backend-neutral queue-depth driven bandwidth loop. The caller supplies direct
// post/wait functions; this helper owns only warmup/window accounting and result
// finalization. There are no virtual calls, heap callbacks, or transport types in
// the hot path.
template <class Post, class Wait>
bandwidth_window_summary run_bandwidth_window(result& r, options const& opt,
                                              std::size_t slots,
                                              Post&& post, Wait&& wait) {
  window_controller win(opt, static_cast<std::size_t>(opt.iterations));
  auto cpu_begin = take_cpu_snapshot();
  std::uint64_t posted = 0;
  std::uint64_t completed = 0;

  auto submit = [&](std::size_t slot) {
    win.note_post();
    if (win.take_opened()) cpu_begin = take_cpu_snapshot();
    post(slot);
    ++posted;
    r.posted_count = posted;
  };

  auto const prime = prime_count(opt, slots);
  for (std::size_t slot = 0; slot < prime; ++slot) submit(slot);

  while (!(win.opened() && win.window_done())) {
    auto const slot = static_cast<std::size_t>(completed % slots);
    wait(slot);
    win.note_complete_bw();
    if (win.take_opened()) cpu_begin = take_cpu_snapshot();
    ++completed;
    if (win.should_post()) submit(slot);
  }

  while (completed < posted) {
    auto const slot = static_cast<std::size_t>(completed % slots);
    wait(slot);
    ++completed;
  }

  std::vector<cycles_t> tp, tc;
  win.bw_arrays(tp, tc);
  finalize_counts(r, win);
  finish_bw_cycles(r, tp, tc, opt.post_list, opt.cq_mod, opt.no_peak);
  fill_cpu_metrics(r, cpu_begin, take_cpu_snapshot());
  return {posted, completed};
}

struct latency_window_summary {
  std::uint64_t completed = 0;
};

// Backend-neutral one-outstanding latency loop. Operation performs exactly one
// measured round trip / one-sided op and blocks until completion.
template <class Operation>
latency_window_summary run_latency_window(result& r, options const& opt,
                                          Operation&& operation,
                                          int rtt_factor) {
  window_controller win(opt, static_cast<std::size_t>(opt.iterations));
  auto cpu_begin = take_cpu_snapshot();
  std::uint64_t completed = 0;

  while (!(win.opened() && win.window_done())) {
    auto const begin = get_cycles();
    operation();
    auto const end = get_cycles();
    ++completed;
    r.posted_count = completed;
    win.note_complete_lat(end - begin);
    if (win.take_opened()) cpu_begin = take_cpu_snapshot();
  }

  finalize_counts(r, win);
  finish_throughput(r, win.window_begin_wall(), window_controller::clock_type::now());
  fill_latency_cycles(r, win.lat_deltas(), rtt_factor);
  fill_cpu_metrics(r, cpu_begin, take_cpu_snapshot());
  return {completed};
}

}  // namespace rdma_bench
