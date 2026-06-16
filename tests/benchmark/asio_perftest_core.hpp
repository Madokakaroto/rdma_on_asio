#pragma once

// Shared core for the RDMA-on-Asio performance benches (the "asio side" of the
// perftest comparison). Stage 9a extracts the pieces that are byte-identical
// across send_recv.cpp and read_write.cpp here, with no behavior change, so the
// two benches can converge into one perftest-shaped tool. It grows over the
// stage; for now it holds the common type aliases and the two verbatim-shared
// data-path helpers (cq_spinner, signal_ready).

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <tuple>

#include <string>

#include "asio/as_tuple.hpp"
#include "asio/error_code.hpp"
#include "asio/use_future.hpp"

#include "rdma/rdma.hpp"
#include "rdma_bench_common.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
using clock_type = std::chrono::steady_clock;
inline constexpr auto use_fut = asio::as_tuple(asio::use_future);
using future_result = std::future<std::tuple<asio::error_code, std::size_t>>;

// Busy-polls a standalone completion queue on a dedicated thread. Poll-mode data
// path: completions fire inline on this thread (non-io_context token), so the
// measured path never touches an io_context. Stops and joins on destruction.
class cq_spinner {
public:
  explicit cq_spinner(rdma::rdma_completion_queue& cq)
      : cq_(cq),
        thread_([this] {
          while (!stop_.load(std::memory_order_relaxed)) {
            asio::error_code ec;
            cq_.poll(ec);
          }
        }) {}

  ~cq_spinner() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
  }

  cq_spinner(cq_spinner const&) = delete;
  cq_spinner& operator=(cq_spinner const&) = delete;

private:
  rdma::rdma_completion_queue& cq_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// Fulfils a readiness promise once (single-process server/client startup sync).
// Tolerates a double-set (the future may already be satisfied on a fast path).
inline void signal_ready(std::promise<void>* ready) {
  if (!ready) return;
  try {
    ready->set_value();
  } catch (std::future_error const&) {
  }
}

namespace asio_perftest {

// Verb-module entry points. Each lives in its own translation unit
// (send_recv.cpp / read_write.cpp) with internal linkage for everything else, so
// the unified asio_perftest binary links both without symbol collisions. Each
// assumes its operation has already been selected; it dispatches mode x metric.
int run_send_recv(rdma_bench::options opt, std::string command_line);
int run_read_write(rdma_bench::options opt, std::string command_line);

// Operation router: send_recv -> run_send_recv; write/read -> run_read_write.
// Defined in asio_perftest.cpp.
int dispatch(rdma_bench::options opt, std::string command_line);

// Shared CLI entry: parse options (+ scenario), optionally force operation/metric
// (the perftest-shaped entrypoints asio_send_bw etc. pass a preset), dispatch.
// preset_op / preset_metric are null for the multiplexed asio_perftest tool.
// Defined in asio_perftest.cpp.
int cli_main(int argc, char** argv, char const* preset_op = nullptr,
             char const* preset_metric = nullptr);

}  // namespace asio_perftest
