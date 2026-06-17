#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"

#include "rdma/rdma.hpp"
#include "rdma_bench_common.hpp"
#include "asio_perftest_core.hpp"

// Stage 9a: this translation unit is now a module of the unified asio_perftest
// binary (it no longer owns main). Everything except the exported run_send_recv
// entry point has internal linkage so send_recv.cpp and read_write.cpp can link
// into one executable without symbol collisions.
namespace {

class event_bw_server : public std::enable_shared_from_this<event_bw_server> {
public:
  using done_handler = std::function<void(rdma_bench::result)>;

  event_bw_server(asio::io_context& io, rdma::rdma_device_ptr device,
                  rdma_bench::options opt, std::string command_line,
                  done_handler on_done)
      : io_(io),
        device_(std::move(device)),
        opt_(std::move(opt)),
        result_(make_base_result(opt_, std::move(command_line))),
        listener_(io),
        conn_(io),
        qp_(io),
        win_(opt_, static_cast<std::size_t>(opt_.iterations)),
        on_done_(std::move(on_done)) {
    result_.scenario_name = "send_recv_server";
  }

  void start() {
    try {
      listener_.open(tcp::v4());
      listener_.bind(opt_.port);
      listener_.listen();
      std::cout << "RDMA_BENCH_READY role=server port=" << opt_.port << "\n";
      listener_.async_get_connection(
          conn_, asio::mutable_buffer{},
          [self = shared_from_this()](asio::error_code ec, std::size_t) {
            self->on_connection(ec);
          });
    } catch (std::exception const& e) {
      fail(e.what());
    }
  }

private:
  void on_connection(asio::error_code ec) {
    if (ec) return fail(ec.message());
    conn_.async_accept(qp_, asio::const_buffer{},
                       [self = shared_from_this()](asio::error_code ec) {
                         self->on_accept(ec);
                       });
  }

  void on_accept(asio::error_code ec) {
    if (ec) return fail(ec.message());
    setup_buffers();
    auto const prime =
        win_.duration_mode()
            ? static_cast<std::uint64_t>(opt_.queue_depth)
            : std::min<std::uint64_t>(
                  opt_.queue_depth, opt_.warmup_iterations + opt_.iterations);
    for (std::uint64_t i = 0; i < prime; ++i) {
      post_recv(static_cast<std::size_t>(i));
    }
    send_ready();
  }

  void setup_buffers() {
    storage_.assign(1 + opt_.message_size * opt_.queue_depth, 0);
    mr_ = std::make_unique<rdma::rdma_memory_region>(
        device_, storage_.data(), storage_.size());
  }

  std::size_t offset_for_slot(std::size_t slot) const {
    return 1 + slot * opt_.message_size;
  }

  void send_ready() {
    storage_[0] = 'R';
    qp_.async_send(mr_->cslice(std::size_t{0}, std::size_t{1}),
                   [self = shared_from_this()](asio::error_code ec,
                                                std::size_t) {
                     if (ec) self->fail(ec.message());
                   });
  }

  void post_recv(std::size_t slot) {
    ++posted_;
    result_.posted_count = posted_;
    qp_.async_recv(
        mr_->slice(offset_for_slot(slot), opt_.message_size),
        [self = shared_from_this(), slot](asio::error_code ec, std::size_t n) {
          self->on_recv(slot, ec, n);
        });
  }

  void on_recv(std::size_t slot, asio::error_code ec, std::size_t n) {
    if (done_) return;
    if (ec) return fail(ec.message());
    if (n != opt_.message_size) {
      if (win_.duration_mode()) return finish_success();  // 1-byte sentinel
      return fail("short receive");
    }
    win_.note_complete_bw();
    ++completed_;
    std::uint64_t const total = opt_.warmup_iterations + opt_.iterations;
    if (win_.duration_mode() || posted_ < total) {
      post_recv(slot);
    }
    if (!win_.duration_mode() && completed_ >= total) {
      finish_success();
    }
  }

  void finish_success() {
    if (done_) return;
    done_ = true;
    rdma_bench::finalize_counts(result_, win_);
    finish_throughput(result_, win_.window_begin_wall(), clock_type::now());
    result_.validation_passed = true;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  void fail(std::string message) {
    if (done_) return;
    done_ = true;
    result_.errors = 1;
    result_.first_error = std::move(message);
    result_.exit_code = 1;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  asio::io_context& io_;
  rdma::rdma_device_ptr device_;
  rdma_bench::options opt_;
  rdma_bench::result result_;
  rdma::rdma_listener<tcp> listener_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  rdma_bench::window_controller win_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  std::uint64_t posted_ = 0;
  std::uint64_t completed_ = 0;
  bool done_ = false;
  done_handler on_done_;
};

class event_bw_client : public std::enable_shared_from_this<event_bw_client> {
public:
  using done_handler = std::function<void(rdma_bench::result)>;

  event_bw_client(asio::io_context& io, rdma::rdma_device_ptr device,
                  rdma_bench::options opt, std::string command_line,
                  done_handler on_done)
      : io_(io),
        device_(std::move(device)),
        opt_(std::move(opt)),
        result_(make_base_result(opt_, std::move(command_line))),
        conn_(io),
        qp_(io),
        win_(opt_, static_cast<std::size_t>(opt_.iterations)),
        on_done_(std::move(on_done)) {
    result_.scenario_name = "send_recv_client";
  }

  void start() {
    try {
      conn_.open(tcp::v4());
      tcp::endpoint endpoint(asio::ip::make_address(opt_.peer_addr), opt_.port);
      conn_.async_connect(qp_, endpoint, asio::const_buffer{},
                          asio::mutable_buffer{},
                          [self = shared_from_this()](asio::error_code ec,
                                                       std::size_t) {
                            self->on_connect(ec);
                          });
    } catch (std::exception const& e) {
      fail(e.what());
    }
  }

private:
  void on_connect(asio::error_code ec) {
    if (ec) return fail(ec.message());
    setup_buffers();
    qp_.async_recv(mr_->slice(std::size_t{0}, std::size_t{1}),
                   [self = shared_from_this()](asio::error_code ec,
                                                std::size_t n) {
                     self->on_ready(ec, n);
                   });
  }

  void setup_buffers() {
    storage_.assign(1 + opt_.message_size * opt_.queue_depth, 0);
    for (std::size_t i = 1; i < storage_.size(); ++i) {
      storage_[i] = static_cast<char>(i & 0x7f);
    }
    mr_ = std::make_unique<rdma::rdma_memory_region>(
        device_, storage_.data(), storage_.size());
  }

  std::size_t offset_for_slot(std::size_t slot) const {
    return 1 + slot * opt_.message_size;
  }

  void on_ready(asio::error_code ec, std::size_t n) {
    if (ec) return fail(ec.message());
    if (n != 1 || storage_[0] != 'R') return fail("missing server ready byte");
    cpu_begin_ = rdma_bench::take_cpu_snapshot();
    auto const prime =
        win_.duration_mode()
            ? static_cast<std::uint64_t>(opt_.queue_depth)
            : std::min<std::uint64_t>(
                  opt_.queue_depth, opt_.warmup_iterations + opt_.iterations);
    for (std::uint64_t i = 0; i < prime; ++i) {
      post_send(static_cast<std::size_t>(i));
    }
  }

  void post_send(std::size_t slot) {
    win_.note_post();
    ++posted_;
    result_.posted_count = posted_;
    qp_.async_send(
        mr_->cslice(offset_for_slot(slot), opt_.message_size),
        [self = shared_from_this(), slot](asio::error_code ec, std::size_t n) {
          self->on_send(slot, ec, n);
        });
  }

  void on_send(std::size_t slot, asio::error_code ec, std::size_t n) {
    if (done_) return;
    if (ec) return fail(ec.message());
    if (n != opt_.message_size) return fail("short send");
    win_.note_complete_bw();
    if (win_.take_opened()) cpu_begin_ = rdma_bench::take_cpu_snapshot();
    ++completed_;
    if (win_.should_post()) {
      post_send(slot);
      return;
    }
    // Window done: once all in-flight ops drain, finish. Duration mode first
    // sends a 1-byte end-of-stream sentinel so the server stops.
    if (completed_ >= posted_) {
      if (win_.duration_mode()) {
        send_sentinel();
      } else {
        finish_success();
      }
    }
  }

  void send_sentinel() {
    if (done_) return;
    storage_[0] = 'E';
    qp_.async_send(mr_->cslice(std::size_t{0}, std::size_t{1}),
                   [self = shared_from_this()](asio::error_code, std::size_t) {
                     self->finish_success();
                   });
  }

  void finish_success() {
    if (done_) return;
    done_ = true;
    std::vector<rdma_bench::cycles_t> tp, tc;
    win_.bw_arrays(tp, tc);
    rdma_bench::finalize_counts(result_, win_);
    rdma_bench::finish_bw_cycles(result_, tp, tc, opt_.post_list, opt_.cq_mod,
                                 opt_.no_peak);
    rdma_bench::fill_cpu_metrics(result_, cpu_begin_,
                                 rdma_bench::take_cpu_snapshot());
    result_.validation_passed = true;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  void fail(std::string message) {
    if (done_) return;
    done_ = true;
    result_.errors = 1;
    result_.first_error = std::move(message);
    result_.exit_code = 1;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  asio::io_context& io_;
  rdma::rdma_device_ptr device_;
  rdma_bench::options opt_;
  rdma_bench::result result_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  rdma_bench::window_controller win_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  std::uint64_t posted_ = 0;
  std::uint64_t completed_ = 0;
  rdma_bench::cpu_snapshot cpu_begin_{};
  bool done_ = false;
  done_handler on_done_;
};

class event_latency_server
    : public std::enable_shared_from_this<event_latency_server> {
public:
  using done_handler = std::function<void(rdma_bench::result)>;

  event_latency_server(asio::io_context& io, rdma::rdma_device_ptr device,
                       rdma_bench::options opt, std::string command_line,
                       done_handler on_done)
      : io_(io),
        device_(std::move(device)),
        opt_(std::move(opt)),
        result_(make_base_result(opt_, std::move(command_line))),
        listener_(io),
        conn_(io),
        qp_(io),
        on_done_(std::move(on_done)) {
    result_.scenario_name = "send_recv_latency_server";
  }

  void start() {
    try {
      listener_.open(tcp::v4());
      listener_.bind(opt_.port);
      listener_.listen();
      std::cout << "RDMA_BENCH_READY role=server port=" << opt_.port << "\n";
      listener_.async_get_connection(
          conn_, asio::mutable_buffer{},
          [self = shared_from_this()](asio::error_code ec, std::size_t) {
            self->on_connection(ec);
          });
    } catch (std::exception const& e) {
      fail(e.what());
    }
  }

private:
  void on_connection(asio::error_code ec) {
    if (ec) return fail(ec.message());
    conn_.async_accept(qp_, asio::const_buffer{},
                       [self = shared_from_this()](asio::error_code ec) {
                         self->on_accept(ec);
                       });
  }

  void on_accept(asio::error_code ec) {
    if (ec) return fail(ec.message());
    storage_.assign(1 + opt_.message_size * 2, 0);
    mr_ = std::make_unique<rdma::rdma_memory_region>(
        device_, storage_.data(), storage_.size());
    post_recv(0);
    storage_[0] = 'R';
    qp_.async_send(mr_->cslice(std::size_t{0}, std::size_t{1}),
                   [self = shared_from_this()](asio::error_code ec,
                                                std::size_t) {
                     if (ec) self->fail(ec.message());
                   });
  }

  std::size_t slot_offset(std::size_t slot) const noexcept {
    return 1 + slot * opt_.message_size;
  }

  void post_recv(std::size_t slot) {
    ++result_.posted_count;
    qp_.async_recv(mr_->slice(slot_offset(slot), opt_.message_size),
                   [self = shared_from_this(), slot](asio::error_code ec,
                                                     std::size_t n) {
                     self->on_recv(slot, ec, n);
                   });
  }

  void on_recv(std::size_t slot, asio::error_code ec, std::size_t n) {
    if (done_) return;
    if (ec) return fail(ec.message());
    std::uint64_t const total = opt_.warmup_iterations + opt_.iterations;
    bool const duration = opt_.duration_sec > 0.0;
    if (n != opt_.message_size) {
      // Duration mode: the client's 1-byte sentinel ends the stream (the server
      // cannot know the round-trip count).
      if (duration) {
        result_.validation_passed = true;
        return finish_success();
      }
      return fail("short receive");
    }
    bool const more = duration || (result_.completed_count + 1 < total);
    if (more) {
      post_recv(1 - slot);
    }
    qp_.async_send(mr_->cslice(slot_offset(slot), opt_.message_size),
                   [self = shared_from_this()](asio::error_code ec,
                                                std::size_t n) {
                     self->on_send(ec, n);
                   });
  }

  void on_send(asio::error_code ec, std::size_t n) {
    if (done_) return;
    if (ec) return fail(ec.message());
    if (n != opt_.message_size) return fail("short send");
    ++result_.completed_count;
    std::uint64_t const total = opt_.warmup_iterations + opt_.iterations;
    if (opt_.duration_sec <= 0.0 && result_.completed_count == total) {
      result_.validation_passed = true;
      conn_.async_wait_disconnect(
          [self = shared_from_this()](asio::error_code) {
            self->finish_success();
          });
      return;
    }
    // The next receive was already posted before sending this echo.
  }

  void finish_success() {
    if (done_) return;
    done_ = true;
    result_.payload_bytes = result_.completed_count * opt_.message_size;
    on_done_(result_);
  }

  void fail(std::string message) {
    if (done_) return;
    done_ = true;
    result_.errors = 1;
    result_.first_error = std::move(message);
    result_.exit_code = 1;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  asio::io_context& io_;
  rdma::rdma_device_ptr device_;
  rdma_bench::options opt_;
  rdma_bench::result result_;
  rdma::rdma_listener<tcp> listener_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  bool done_ = false;
  done_handler on_done_;
};

class event_latency_client
    : public std::enable_shared_from_this<event_latency_client> {
public:
  using done_handler = std::function<void(rdma_bench::result)>;

  event_latency_client(asio::io_context& io, rdma::rdma_device_ptr device,
                       rdma_bench::options opt, std::string command_line,
                       done_handler on_done)
      : io_(io),
        device_(std::move(device)),
        opt_(std::move(opt)),
        result_(make_base_result(opt_, std::move(command_line))),
        conn_(io),
        qp_(io),
        win_(opt_, static_cast<std::size_t>(opt_.iterations)),
        on_done_(std::move(on_done)) {
    result_.scenario_name = "send_recv_latency_client";
  }

  void start() {
    try {
      conn_.open(tcp::v4());
      tcp::endpoint endpoint(asio::ip::make_address(opt_.peer_addr), opt_.port);
      conn_.async_connect(qp_, endpoint, asio::const_buffer{},
                          asio::mutable_buffer{},
                          [self = shared_from_this()](asio::error_code ec,
                                                       std::size_t) {
                            self->on_connect(ec);
                          });
    } catch (std::exception const& e) {
      fail(e.what());
    }
  }

private:
  void on_connect(asio::error_code ec) {
    if (ec) return fail(ec.message());
    storage_.assign(1 + opt_.message_size * 2, 0);
    for (std::size_t i = 1; i < 1 + opt_.message_size; ++i) {
      storage_[i] = static_cast<char>(i & 0x7f);
    }
    mr_ = std::make_unique<rdma::rdma_memory_region>(
        device_, storage_.data(), storage_.size());
    qp_.async_recv(mr_->slice(std::size_t{0}, std::size_t{1}),
                   [self = shared_from_this()](asio::error_code ec,
                                                std::size_t n) {
                     self->on_ready(ec, n);
                   });
  }

  void on_ready(asio::error_code ec, std::size_t n) {
    if (ec) return fail(ec.message());
    if (n != 1 || storage_[0] != 'R') return fail("missing server ready byte");
    cpu_begin_ = rdma_bench::take_cpu_snapshot();
    start_iteration();
  }

  void start_iteration() {
    sample_begin_cyc_ = rdma_bench::get_cycles();
    ++result_.posted_count;
    qp_.async_recv(mr_->slice(recv_offset(), opt_.message_size),
                   [self = shared_from_this()](asio::error_code ec,
                                                std::size_t n) {
                     self->on_echo(ec, n);
                   });
    qp_.async_send(mr_->cslice(send_offset(), opt_.message_size),
                   [self = shared_from_this()](asio::error_code ec,
                                                std::size_t n) {
                     self->on_send(ec, n);
                   });
  }

  void on_send(asio::error_code ec, std::size_t n) {
    if (done_) return;
    if (ec) return fail(ec.message());
    if (n != opt_.message_size) return fail("short send");
  }

  void on_echo(asio::error_code ec, std::size_t n) {
    if (done_) return;
    auto sample_end_cyc = rdma_bench::get_cycles();
    if (ec) return fail(ec.message());
    if (n != opt_.message_size) return fail("short receive");
    win_.note_complete_lat(sample_end_cyc - sample_begin_cyc_);
    if (win_.take_opened()) cpu_begin_ = rdma_bench::take_cpu_snapshot();
    if (win_.opened() && win_.window_done()) {
      // Duration mode first sends a 1-byte sentinel so the echo server stops.
      if (win_.duration_mode()) {
        send_sentinel();
      } else {
        finish_success();
      }
      return;
    }
    start_iteration();
  }

  void send_sentinel() {
    if (done_) return;
    storage_[0] = 'E';
    qp_.async_send(mr_->cslice(std::size_t{0}, std::size_t{1}),
                   [self = shared_from_this()](asio::error_code, std::size_t) {
                     self->finish_success();
                   });
  }

  void finish_success() {
    if (done_) return;
    done_ = true;
    rdma_bench::finalize_counts(result_, win_);
    finish_throughput(result_, win_.window_begin_wall(), clock_type::now());
    rdma_bench::fill_cpu_metrics(result_, cpu_begin_,
                                 rdma_bench::take_cpu_snapshot());
    rdma_bench::fill_latency_cycles(result_, win_.lat_deltas(), 2);
    result_.validation_passed = true;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  void fail(std::string message) {
    if (done_) return;
    done_ = true;
    result_.errors = 1;
    result_.first_error = std::move(message);
    result_.exit_code = 1;
    asio::error_code ignored;
    conn_.disconnect(ignored);
    on_done_(result_);
  }

  asio::io_context& io_;
  rdma::rdma_device_ptr device_;
  rdma_bench::options opt_;
  rdma_bench::result result_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  rdma_bench::window_controller win_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  rdma_bench::cycles_t sample_begin_cyc_ = 0;
  rdma_bench::cpu_snapshot cpu_begin_{};
  bool done_ = false;
  done_handler on_done_;

  static constexpr std::size_t send_offset() noexcept { return 1; }

  std::size_t recv_offset() const noexcept {
    return 1 + opt_.message_size;
  }
};

int run_event_bandwidth(rdma_bench::options opt, std::string command_line) {
  asio::io_context io;
  auto device =
      rdma::rdma_device_manager_t::instance().get_first_available_device(tcp::v4(), {});
  rdma::use_device(io, device);

  std::vector<rdma_bench::result> results;
  std::atomic<int> remaining{0};
  auto done = [&](rdma_bench::result r) {
    results.push_back(std::move(r));
    if (--remaining == 0) io.stop();
  };

  if (opt.single_process) {
    remaining = 2;
    auto server_opt = opt;
    server_opt.topology = "single_host_same_process";
    auto client_opt = opt;
    client_opt.client = true;
    client_opt.peer_addr = opt.local_addr;
    client_opt.topology = "single_host_same_process";
    std::make_shared<event_bw_server>(io, device, server_opt, command_line, done)
        ->start();
    std::make_shared<event_bw_client>(io, device, client_opt, command_line, done)
        ->start();
  } else if (opt.server) {
    remaining = 1;
    std::make_shared<event_bw_server>(io, device, opt, command_line, done)->start();
  } else {
    remaining = 1;
    std::make_shared<event_bw_client>(io, device, opt, command_line, done)->start();
  }

  asio::steady_timer watchdog(io);
  watchdog.expires_after(std::chrono::seconds(opt.timeout_sec));
  watchdog.async_wait([&](asio::error_code ec) {
    if (!ec) {
      rdma_bench::result r = make_base_result(opt, command_line);
      r.errors = 1;
      r.first_error = "watchdog timeout";
      r.exit_code = 2;
      results.push_back(std::move(r));
      io.stop();
    }
  });

  io.run();
  watchdog.cancel();

  auto selected = std::find_if(results.begin(), results.end(),
                               [](auto const& r) {
                                 return r.errors == 0 &&
                                        r.scenario_name == "send_recv_client";
                               });
  if (selected == results.end()) {
    selected = std::find_if(results.begin(), results.end(),
                            [](auto const& r) { return r.errors == 0; });
  }
  if (selected == results.end() && !results.empty()) selected = results.begin();
  if (selected == results.end()) {
    auto r = make_base_result(opt, command_line);
    r.errors = 1;
    r.first_error = "no result produced";
    r.exit_code = 1;
    rdma_bench::write_result(r, opt.json_out);
    return r.exit_code;
  }
  rdma_bench::write_result(*selected, opt.json_out);
  return selected->exit_code;
}

int run_event_latency(rdma_bench::options opt, std::string command_line) {
  if (opt.token_type != "callback") {
    auto r = make_base_result(opt, command_line);
    r.skip_reason =
        "latency use_awaitable path is not enabled in the first benchmark stage";
    r.missing_capability = "latency_token_" + opt.token_type;
    r.exit_code = 0;
    rdma_bench::write_result(r, opt.json_out);
    return 0;
  }

  asio::io_context io;
  auto device =
      rdma::rdma_device_manager_t::instance().get_first_available_device(tcp::v4(), {});
  rdma::use_device(io, device);

  std::vector<rdma_bench::result> results;
  std::atomic<int> remaining{0};
  auto done = [&](rdma_bench::result r) {
    results.push_back(std::move(r));
    if (--remaining == 0) io.stop();
  };

  if (opt.single_process) {
    remaining = 2;
    auto server_opt = opt;
    server_opt.topology = "single_host_same_process";
    auto client_opt = opt;
    client_opt.peer_addr = opt.local_addr;
    client_opt.topology = "single_host_same_process";
    std::make_shared<event_latency_server>(io, device, server_opt, command_line,
                                           done)
        ->start();
    std::make_shared<event_latency_client>(io, device, client_opt, command_line,
                                           done)
        ->start();
  } else if (opt.server) {
    remaining = 1;
    std::make_shared<event_latency_server>(io, device, opt, command_line, done)
        ->start();
  } else {
    remaining = 1;
    std::make_shared<event_latency_client>(io, device, opt, command_line, done)
        ->start();
  }

  asio::steady_timer watchdog(io);
  watchdog.expires_after(std::chrono::seconds(opt.timeout_sec));
  watchdog.async_wait([&](asio::error_code ec) {
    if (!ec) {
      rdma_bench::result r = make_base_result(opt, command_line);
      r.errors = 1;
      r.first_error = "watchdog timeout";
      r.exit_code = 2;
      results.push_back(std::move(r));
      io.stop();
    }
  });

  io.run();
  watchdog.cancel();

  auto selected = std::find_if(results.begin(), results.end(),
                               [](auto const& r) {
                                 return r.errors == 0 &&
                                        r.latency_avg_us.has_value() &&
                                        r.scenario_name ==
                                            "send_recv_latency_client";
                               });
  if (selected == results.end()) {
    selected = std::find_if(results.begin(), results.end(),
                            [](auto const& r) { return r.errors == 0; });
  }
  if (selected == results.end() && !results.empty()) selected = results.begin();
  if (selected == results.end()) {
    auto r = make_base_result(opt, command_line);
    r.errors = 1;
    r.first_error = "no result produced";
    r.exit_code = 1;
    rdma_bench::write_result(r, opt.json_out);
    return r.exit_code;
  }
  rdma_bench::write_result(*selected, opt.json_out);
  return selected->exit_code;
}

rdma_bench::result failed_poll_result(rdma_bench::options const& opt,
                                      std::string const& command_line,
                                      std::string message) {
  auto r = make_base_result(opt, command_line);
  r.errors = 1;
  r.first_error = std::move(message);
  r.exit_code = 1;
  return r;
}

rdma_bench::result run_poll_callback_bandwidth_server_role(
    rdma_bench::options opt, std::string command_line,
    std::promise<void>* ready) {
  opt.server = true;
  opt.client = false;
  opt.single_process = false;
  auto result = make_base_result(opt, command_line);
  result.scenario_name = "send_recv_poll_callback_server";

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    rdma::rdma_listener<tcp> listener(io);
    listener.open(tcp::v4());
    listener.bind(opt.port);
    listener.listen();
    std::cout << "RDMA_BENCH_READY role=server mode=poll token=callback port="
              << opt.port << "\n";
    signal_ready(ready);

    rdma::rdma_connector<tcp> conn(io);
    asio::error_code get_ec;
    std::size_t req_len = 0;
    listener.async_get_connection(
        conn, asio::mutable_buffer{},
        [&](asio::error_code ec, std::size_t n) {
          get_ec = ec;
          req_len = n;
        });
    io.run();
    io.restart();
    (void)req_len;
    if (get_ec) return failed_poll_result(opt, command_line, get_ec.message());

    rdma::rdma_completion_queue cq(device);
    rdma::rdma_queue_pair qp(cq);
    asio::error_code accept_ec;
    conn.async_accept(qp, asio::const_buffer{},
                      [&](asio::error_code ec) { accept_ec = ec; });
    io.run();
    io.restart();
    if (accept_ec) {
      return failed_poll_result(opt, command_line, accept_ec.message());
    }

    std::size_t const slots = static_cast<std::size_t>(
        std::max<std::uint64_t>(
            1, std::min<std::uint64_t>(
                   static_cast<std::uint64_t>(opt.queue_depth),
                   opt.iterations)));
    std::vector<char> storage(1 + opt.message_size * slots, 0);
    rdma::rdma_memory_region mr(device, storage.data(), storage.size());
    auto offset_for_slot = [&](std::size_t slot) {
      return 1 + slot * opt.message_size;
    };

    std::string error_message;
    bool failed = false;
    auto fail = [&](std::string message) {
      if (!failed) {
        failed = true;
        error_message = std::move(message);
      }
    };

    rdma_bench::window_controller win(
        opt, static_cast<std::size_t>(opt.iterations));
    std::uint64_t posted = 0, completed = 0;
    std::uint64_t const total = opt.warmup_iterations + opt.iterations;
    bool stop = false;  // duration: set when the client's end-of-stream arrives
    auto cpu_begin = rdma_bench::take_cpu_snapshot();
    std::function<void(std::size_t)> post_recv;
    post_recv = [&](std::size_t slot) {
      ++posted;
      result.posted_count = posted;
      qp.async_recv(
          mr.slice(offset_for_slot(slot), opt.message_size),
          [&, slot](asio::error_code ec, std::size_t n) {
            if (failed) return;
            if (ec) return fail(ec.message());
            if (n != opt.message_size) {
              if (win.duration_mode()) {  // 1-byte end-of-stream sentinel
                stop = true;
                return;
              }
              return fail("short receive");
            }
            win.note_complete_bw();
            if (win.take_opened()) cpu_begin = rdma_bench::take_cpu_snapshot();
            ++completed;
            bool const more = win.duration_mode() ? !stop : (posted < total);
            if (more) post_recv(slot);
          });
    };

    auto const prime =
        win.duration_mode() ? slots : std::min<std::uint64_t>(slots, total);
    for (std::uint64_t i = 0; i < prime; ++i) {
      post_recv(static_cast<std::size_t>(i));
    }

    bool ready_send_done = false;
    storage[0] = 'R';
    qp.async_send(mr.cslice(std::size_t{0}, std::size_t{1}),
                  [&](asio::error_code ec, std::size_t n) {
                    if (ec) fail(ec.message());
                    else if (n != 1) fail("short ready send");
                    ready_send_done = true;
                  });
    if (!poll_until(cq, [&] { return ready_send_done || failed; },
                    std::chrono::seconds(opt.timeout_sec), error_message)) {
      return failed_poll_result(opt, command_line, error_message);
    }
    if (failed) return failed_poll_result(opt, command_line, error_message);

    // iters: serve warmup+iters. duration: serve until the client's sentinel.
    if (!poll_until(cq,
                    [&] {
                      return failed ||
                             (win.duration_mode() ? stop : completed >= total);
                    },
                    std::chrono::seconds(opt.timeout_sec), error_message)) {
      return failed_poll_result(opt, command_line, error_message);
    }
    if (failed) return failed_poll_result(opt, command_line, error_message);

    rdma_bench::finalize_counts(result, win);
    finish_throughput(result, win.window_begin_wall(), clock_type::now());
    rdma_bench::fill_cpu_metrics(result, cpu_begin,
                                 rdma_bench::take_cpu_snapshot());
    result.validation_passed = true;
    asio::error_code ignored;
    conn.disconnect(ignored);
    return result;
  } catch (std::exception const& e) {
    signal_ready(ready);
    return failed_poll_result(opt, command_line, e.what());
  }
}

rdma_bench::result run_poll_callback_bandwidth_client_role(
    rdma_bench::options opt, std::string command_line) {
  opt.client = true;
  opt.server = false;
  opt.single_process = false;
  auto result = make_base_result(opt, command_line);
  result.scenario_name = "send_recv_poll_callback_client";

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    rdma::rdma_completion_queue cq(device);
    rdma::rdma_connector<tcp> conn(io);
    conn.open(tcp::v4());
    rdma::rdma_queue_pair qp(cq);
    tcp::endpoint endpoint(asio::ip::make_address(opt.peer_addr), opt.port);
    asio::error_code connect_ec;
    std::size_t reply_len = 0;
    conn.async_connect(qp, endpoint, asio::const_buffer{},
                       asio::mutable_buffer{},
                       [&](asio::error_code ec, std::size_t n) {
                         connect_ec = ec;
                         reply_len = n;
                       });
    io.run();
    io.restart();
    (void)reply_len;
    if (connect_ec) {
      return failed_poll_result(opt, command_line, connect_ec.message());
    }

    std::size_t const slots = static_cast<std::size_t>(
        std::max<std::uint64_t>(
            1, std::min<std::uint64_t>(
                   static_cast<std::uint64_t>(opt.queue_depth),
                   opt.iterations)));
    std::vector<char> storage(1 + opt.message_size * slots, 0);
    for (std::size_t i = 1; i < storage.size(); ++i) {
      storage[i] = static_cast<char>(i & 0x7f);
    }
    rdma::rdma_memory_region mr(device, storage.data(), storage.size());
    auto offset_for_slot = [&](std::size_t slot) {
      return 1 + slot * opt.message_size;
    };

    std::string error_message;
    bool failed = false;
    auto fail = [&](std::string message) {
      if (!failed) {
        failed = true;
        error_message = std::move(message);
      }
    };

    bool ready_recv_done = false;
    qp.async_recv(mr.slice(std::size_t{0}, std::size_t{1}),
                  [&](asio::error_code ec, std::size_t n) {
                    if (ec) fail(ec.message());
                    else if (n != 1 || storage[0] != 'R') {
                      fail("missing server ready byte");
                    }
                    ready_recv_done = true;
                  });
    if (!poll_until(cq, [&] { return ready_recv_done || failed; },
                    std::chrono::seconds(opt.timeout_sec), error_message)) {
      return failed_poll_result(opt, command_line, error_message);
    }
    if (failed) return failed_poll_result(opt, command_line, error_message);

    rdma_bench::window_controller win(
        opt, static_cast<std::size_t>(opt.iterations));
    std::uint64_t posted = 0, completed = 0;
    auto cpu_begin = rdma_bench::take_cpu_snapshot();
    std::function<void(std::size_t)> post_send;
    post_send = [&](std::size_t slot) {
      win.note_post();
      ++posted;
      result.posted_count = posted;
      qp.async_send(
          mr.cslice(offset_for_slot(slot), opt.message_size),
          [&, slot](asio::error_code ec, std::size_t n) {
            if (failed) return;
            if (ec) return fail(ec.message());
            if (n != opt.message_size) return fail("short send");
            win.note_complete_bw();
            if (win.take_opened()) cpu_begin = rdma_bench::take_cpu_snapshot();
            ++completed;
            if (win.should_post()) post_send(slot);
          });
    };

    auto const prime = win.duration_mode()
                           ? slots
                           : std::min<std::uint64_t>(
                                 slots, opt.warmup_iterations + opt.iterations);
    for (std::uint64_t i = 0; i < prime; ++i) {
      post_send(static_cast<std::size_t>(i));
    }
    // Timed window: stop once warmup is done and the window (iters count or
    // duration deadline) is complete.
    if (!poll_until(cq,
                    [&] { return failed || (win.opened() && win.window_done()); },
                    std::chrono::seconds(opt.timeout_sec), error_message)) {
      return failed_poll_result(opt, command_line, error_message);
    }
    if (failed) return failed_poll_result(opt, command_line, error_message);
    // Drain ops still in flight (their callbacks no longer repost).
    if (!poll_until(cq, [&] { return failed || completed >= posted; },
                    std::chrono::seconds(opt.timeout_sec), error_message)) {
      return failed_poll_result(opt, command_line, error_message);
    }
    if (failed) return failed_poll_result(opt, command_line, error_message);
    // Duration mode: 1-byte end-of-stream sentinel so the server breaks (the
    // server cannot know the op count; see run_poll_bandwidth_client_role).
    if (win.duration_mode()) {
      storage[0] = 'E';
      bool sent = false;
      qp.async_send(mr.cslice(std::size_t{0}, std::size_t{1}),
                    [&](asio::error_code, std::size_t) { sent = true; });
      poll_until(cq, [&] { return failed || sent; },
                 std::chrono::seconds(opt.timeout_sec), error_message);
    }

    std::vector<rdma_bench::cycles_t> tp, tc;
    win.bw_arrays(tp, tc);
    rdma_bench::finalize_counts(result, win);
    rdma_bench::finish_bw_cycles(result, tp, tc, opt.post_list, opt.cq_mod,
                                 opt.no_peak);
    rdma_bench::fill_cpu_metrics(result, cpu_begin,
                                 rdma_bench::take_cpu_snapshot());
    result.validation_passed = true;
    asio::error_code ignored;
    conn.disconnect(ignored);
    return result;
  } catch (std::exception const& e) {
    return failed_poll_result(opt, command_line, e.what());
  }
}

rdma_bench::result run_poll_bandwidth_server_role(rdma_bench::options opt,
                                                  std::string command_line,
                                                  std::promise<void>* ready) {
  opt.server = true;
  opt.client = false;
  opt.single_process = false;
  auto result = make_base_result(opt, command_line);
  result.scenario_name = "send_recv_poll_server";

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    rdma::rdma_listener<tcp> listener(io);
    listener.open(tcp::v4());
    listener.bind(opt.port);
    listener.listen();
    std::cout << "RDMA_BENCH_READY role=server mode=poll port=" << opt.port
              << "\n";
    signal_ready(ready);

    rdma::rdma_connector<tcp> conn(io);
    asio::error_code get_ec;
    std::size_t req_len = 0;
    listener.async_get_connection(
        conn, asio::mutable_buffer{},
        [&](asio::error_code ec, std::size_t n) {
          get_ec = ec;
          req_len = n;
        });
    io.run();
    io.restart();
    (void)req_len;
    if (get_ec) return failed_poll_result(opt, command_line, get_ec.message());

    rdma::rdma_completion_queue cq(device);
    rdma::rdma_queue_pair qp(cq);
    asio::error_code accept_ec;
    conn.async_accept(qp, asio::const_buffer{},
                      [&](asio::error_code ec) { accept_ec = ec; });
    io.run();
    io.restart();
    if (accept_ec) {
      return failed_poll_result(opt, command_line, accept_ec.message());
    }

    std::vector<char> storage(1 + opt.message_size * opt.queue_depth, 0);
    rdma::rdma_memory_region mr(device, storage.data(), storage.size());
    auto offset_for_slot = [&](std::size_t slot) {
      return 1 + slot * opt.message_size;
    };

    cq_spinner spinner(cq);
    std::vector<future_result> recvs(opt.queue_depth);
    rdma_bench::window_controller win(
        opt, static_cast<std::size_t>(opt.iterations));
    std::uint64_t posted = 0, completed = 0;
    std::uint64_t const total = opt.warmup_iterations + opt.iterations;
    auto const prime = win.duration_mode()
                           ? static_cast<std::uint64_t>(opt.queue_depth)
                           : std::min<std::uint64_t>(opt.queue_depth, total);
    for (std::uint64_t i = 0; i < prime; ++i) {
      auto const slot = static_cast<std::size_t>(i % opt.queue_depth);
      recvs[slot] = qp.async_recv(
          mr.slice(offset_for_slot(slot), opt.message_size), use_fut);
      ++posted;
    }
    result.posted_count = posted;

    storage[0] = 'R';
    auto [ready_ec, ready_n] =
        qp.async_send(mr.cslice(std::size_t{0}, std::size_t{1}), use_fut).get();
    if (ready_ec || ready_n != 1) {
      return failed_poll_result(opt, command_line,
                                ready_ec ? ready_ec.message()
                                         : "short ready send");
    }

    auto cpu_begin = rdma_bench::take_cpu_snapshot();
    // iters: serve warmup+iters recvs. duration: serve until the client
    // disconnects (recv retires with an error / flush) -- the termination
    // barrier, since the server cannot know the duration-mode op count.
    for (;;) {
      if (!win.duration_mode() && completed >= total) break;
      auto const slot = static_cast<std::size_t>(completed % opt.queue_depth);
      auto [ec, n] = recvs[slot].get();
      if (ec || n != opt.message_size) break;  // client done (disconnect/flush)
      win.note_complete_bw();
      if (win.take_opened()) cpu_begin = rdma_bench::take_cpu_snapshot();
      ++completed;
      if (win.duration_mode() || posted < total) {
        recvs[slot] = qp.async_recv(
            mr.slice(offset_for_slot(slot), opt.message_size), use_fut);
        ++posted;
        result.posted_count = posted;
      }
    }

    rdma_bench::finalize_counts(result, win);
    finish_throughput(result, win.window_begin_wall(), clock_type::now());
    rdma_bench::fill_cpu_metrics(result, cpu_begin,
                                 rdma_bench::take_cpu_snapshot());
    result.validation_passed = true;
    asio::error_code ignored;
    conn.disconnect(ignored);
    return result;
  } catch (std::exception const& e) {
    signal_ready(ready);
    return failed_poll_result(opt, command_line, e.what());
  }
}

rdma_bench::result run_poll_bandwidth_client_role(rdma_bench::options opt,
                                                  std::string command_line) {
  opt.client = true;
  opt.server = false;
  opt.single_process = false;
  auto result = make_base_result(opt, command_line);
  result.scenario_name = "send_recv_poll_client";

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    rdma::rdma_completion_queue cq(device);
    rdma::rdma_connector<tcp> conn(io);
    conn.open(tcp::v4());
    rdma::rdma_queue_pair qp(cq);
    tcp::endpoint endpoint(asio::ip::make_address(opt.peer_addr), opt.port);
    asio::error_code connect_ec;
    std::size_t reply_len = 0;
    conn.async_connect(qp, endpoint, asio::const_buffer{},
                       asio::mutable_buffer{},
                       [&](asio::error_code ec, std::size_t n) {
                         connect_ec = ec;
                         reply_len = n;
                       });
    io.run();
    io.restart();
    (void)reply_len;
    if (connect_ec) {
      return failed_poll_result(opt, command_line, connect_ec.message());
    }

    std::vector<char> storage(1 + opt.message_size * opt.queue_depth, 0);
    for (std::size_t i = 1; i < storage.size(); ++i) {
      storage[i] = static_cast<char>(i & 0x7f);
    }
    rdma::rdma_memory_region mr(device, storage.data(), storage.size());
    auto offset_for_slot = [&](std::size_t slot) {
      return 1 + slot * opt.message_size;
    };

    cq_spinner spinner(cq);
    auto [ready_ec, ready_n] =
        qp.async_recv(mr.slice(std::size_t{0}, std::size_t{1}), use_fut).get();
    if (ready_ec || ready_n != 1 || storage[0] != 'R') {
      return failed_poll_result(opt, command_line,
                                ready_ec ? ready_ec.message()
                                         : "missing server ready byte");
    }

    std::vector<future_result> sends(opt.queue_depth);
    // Stage 9b: warmup (first warmup_iterations ops excluded) + duration/margin
    // window via window_controller. iters mode posts warmup+iters total; duration
    // mode posts until the deadline. The window opens once warmup completions
    // retire; cpu snapshot is taken there.
    rdma_bench::window_controller win(
        opt, static_cast<std::size_t>(opt.iterations));
    std::uint64_t posted = 0, completed = 0;
    std::uint64_t const total_posts = opt.warmup_iterations + opt.iterations;
    auto const prime =
        win.duration_mode()
            ? static_cast<std::uint64_t>(opt.queue_depth)
            : std::min<std::uint64_t>(opt.queue_depth, total_posts);
    auto cpu_begin = rdma_bench::take_cpu_snapshot();
    for (std::uint64_t i = 0; i < prime; ++i) {
      auto const slot = static_cast<std::size_t>(i % opt.queue_depth);
      win.note_post();
      sends[slot] = qp.async_send(
          mr.cslice(offset_for_slot(slot), opt.message_size), use_fut);
      ++posted;
    }
    result.posted_count = posted;

    while (!(win.opened() && win.window_done())) {
      auto const slot = static_cast<std::size_t>(completed % opt.queue_depth);
      auto [ec, n] = sends[slot].get();
      if (ec) return failed_poll_result(opt, command_line, ec.message());
      if (n != opt.message_size) {
        return failed_poll_result(opt, command_line, "short send");
      }
      win.note_complete_bw();
      if (win.take_opened()) cpu_begin = rdma_bench::take_cpu_snapshot();
      ++completed;
      if (win.should_post()) {
        win.note_post();
        sends[slot] = qp.async_send(
            mr.cslice(offset_for_slot(slot), opt.message_size), use_fut);
        ++posted;
        result.posted_count = posted;
      }
    }
    // Drain ops still in flight after the window closed (not recorded).
    while (completed < posted) {
      auto const slot = static_cast<std::size_t>(completed % opt.queue_depth);
      auto [ec, n] = sends[slot].get();
      (void)ec;
      (void)n;
      ++completed;
    }
    // Duration mode: the server serves until end-of-stream (it cannot know the
    // op count, and a poll-mode disconnect's DREQ is not processed by the server
    // mid-measure since it does not run its io_context). Send a zero-length
    // sentinel -> the server recv completes with n==0 (!= message_size) and it
    // breaks. iters mode needs none: the server stops on its warmup+iters count.
    if (win.duration_mode()) {
      storage[0] = 'E';
      auto [se, sn] =
          qp.async_send(mr.cslice(std::size_t{0}, std::size_t{1}), use_fut).get();
      (void)se;
      (void)sn;
    }

    std::vector<rdma_bench::cycles_t> tp, tc;
    win.bw_arrays(tp, tc);
    rdma_bench::finalize_counts(result, win);
    rdma_bench::finish_bw_cycles(result, tp, tc, opt.post_list, opt.cq_mod,
                                 opt.no_peak);
    rdma_bench::fill_cpu_metrics(result, cpu_begin,
                                 rdma_bench::take_cpu_snapshot());
    result.validation_passed = true;
    asio::error_code ignored;
    conn.disconnect(ignored);
    return result;
  } catch (std::exception const& e) {
    return failed_poll_result(opt, command_line, e.what());
  }
}

rdma_bench::result run_poll_latency_server_role(rdma_bench::options opt,
                                                std::string command_line,
                                                std::promise<void>* ready) {
  opt.server = true;
  opt.client = false;
  opt.single_process = false;
  auto result = make_base_result(opt, command_line);
  result.scenario_name = "send_recv_poll_latency_server";

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    rdma::rdma_listener<tcp> listener(io);
    listener.open(tcp::v4());
    listener.bind(opt.port);
    listener.listen();
    std::cout << "RDMA_BENCH_READY role=server mode=poll port=" << opt.port
              << "\n";
    signal_ready(ready);

    rdma::rdma_connector<tcp> conn(io);
    asio::error_code get_ec;
    listener.async_get_connection(
        conn, asio::mutable_buffer{},
        [&](asio::error_code ec, std::size_t) { get_ec = ec; });
    io.run();
    io.restart();
    if (get_ec) return failed_poll_result(opt, command_line, get_ec.message());

    rdma::rdma_completion_queue cq(device);
    rdma::rdma_queue_pair qp(cq);
    asio::error_code accept_ec;
    conn.async_accept(qp, asio::const_buffer{},
                      [&](asio::error_code ec) { accept_ec = ec; });
    io.run();
    io.restart();
    if (accept_ec) {
      return failed_poll_result(opt, command_line, accept_ec.message());
    }

    std::vector<char> storage(1 + opt.message_size * 2, 0);
    rdma::rdma_memory_region mr(device, storage.data(), storage.size());
    auto slot_offset = [&](std::size_t slot) {
      return 1 + slot * opt.message_size;
    };

    cq_spinner spinner(cq);
    future_result pending =
        qp.async_recv(mr.slice(slot_offset(0), opt.message_size), use_fut);
    result.posted_count = 1;
    storage[0] = 'R';
    auto [ready_ec, ready_n] =
        qp.async_send(mr.cslice(std::size_t{0}, std::size_t{1}), use_fut).get();
    if (ready_ec || ready_n != 1) {
      return failed_poll_result(opt, command_line,
                                ready_ec ? ready_ec.message()
                                         : "short ready send");
    }

    // iters: echo warmup+iters round-trips. duration: echo until the client's
    // 1-byte sentinel (recv n != message_size) -- the server cannot know the
    // duration-mode round-trip count.
    std::uint64_t const total = opt.warmup_iterations + opt.iterations;
    bool const duration_mode = opt.duration_sec > 0.0;
    std::uint64_t completed = 0;
    for (std::uint64_t i = 0;; ++i) {
      if (!duration_mode && completed >= total) break;
      auto slot = static_cast<std::size_t>(i % 2);
      auto [recv_ec, recv_n] = pending.get();
      if (recv_ec) {
        return failed_poll_result(opt, command_line, recv_ec.message());
      }
      if (recv_n != opt.message_size) {
        if (duration_mode) break;  // 1-byte end-of-stream sentinel
        return failed_poll_result(opt, command_line, "short receive");
      }
      future_result next;
      bool const more = duration_mode || (completed + 1 < total);
      if (more) {
        auto next_slot = std::size_t{1} - slot;
        next = qp.async_recv(mr.slice(slot_offset(next_slot), opt.message_size),
                             use_fut);
        ++result.posted_count;
      }
      auto [send_ec, send_n] =
          qp.async_send(mr.cslice(slot_offset(slot), opt.message_size), use_fut)
              .get();
      if (send_ec) {
        return failed_poll_result(opt, command_line, send_ec.message());
      }
      if (send_n != opt.message_size) {
        return failed_poll_result(opt, command_line, "short send");
      }
      ++completed;
      pending = std::move(next);
    }

    result.completed_count = completed;
    result.payload_bytes = completed * opt.message_size;
    result.validation_passed = true;
    asio::error_code ignored;
    conn.disconnect(ignored);
    return result;
  } catch (std::exception const& e) {
    signal_ready(ready);
    return failed_poll_result(opt, command_line, e.what());
  }
}

rdma_bench::result run_poll_latency_client_role(rdma_bench::options opt,
                                                std::string command_line) {
  opt.client = true;
  opt.server = false;
  opt.single_process = false;
  auto result = make_base_result(opt, command_line);
  result.scenario_name = "send_recv_poll_latency_client";

  try {
    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io, device);

    rdma::rdma_completion_queue cq(device);
    rdma::rdma_connector<tcp> conn(io);
    conn.open(tcp::v4());
    rdma::rdma_queue_pair qp(cq);
    tcp::endpoint endpoint(asio::ip::make_address(opt.peer_addr), opt.port);
    asio::error_code connect_ec;
    conn.async_connect(qp, endpoint, asio::const_buffer{},
                       asio::mutable_buffer{},
                       [&](asio::error_code ec, std::size_t) {
                         connect_ec = ec;
                       });
    io.run();
    io.restart();
    if (connect_ec) {
      return failed_poll_result(opt, command_line, connect_ec.message());
    }

    std::vector<char> storage(1 + opt.message_size * 2, 0);
    for (std::size_t i = 1; i < 1 + opt.message_size; ++i) {
      storage[i] = static_cast<char>(i & 0x7f);
    }
    rdma::rdma_memory_region mr(device, storage.data(), storage.size());
    auto send_offset = std::size_t{1};
    auto recv_offset = std::size_t{1} + opt.message_size;

    cq_spinner spinner(cq);
    auto [ready_ec, ready_n] =
        qp.async_recv(mr.slice(std::size_t{0}, std::size_t{1}), use_fut).get();
    if (ready_ec || ready_n != 1 || storage[0] != 'R') {
      return failed_poll_result(opt, command_line,
                                ready_ec ? ready_ec.message()
                                         : "missing server ready byte");
    }

    rdma_bench::window_controller win(
        opt, static_cast<std::size_t>(opt.iterations));
    auto cpu_begin = rdma_bench::take_cpu_snapshot();
    while (!(win.opened() && win.window_done())) {
      auto sample_begin_cyc = rdma_bench::get_cycles();
      auto recv =
          qp.async_recv(mr.slice(recv_offset, opt.message_size), use_fut);
      auto [send_ec, send_n] =
          qp.async_send(mr.cslice(send_offset, opt.message_size), use_fut).get();
      if (send_ec) {
        return failed_poll_result(opt, command_line, send_ec.message());
      }
      if (send_n != opt.message_size) {
        return failed_poll_result(opt, command_line, "short send");
      }
      auto [recv_ec, recv_n] = recv.get();
      auto sample_end_cyc = rdma_bench::get_cycles();
      if (recv_ec) {
        return failed_poll_result(opt, command_line, recv_ec.message());
      }
      if (recv_n != opt.message_size) {
        return failed_poll_result(opt, command_line, "short receive");
      }
      win.note_complete_lat(sample_end_cyc - sample_begin_cyc);
      if (win.take_opened()) cpu_begin = rdma_bench::take_cpu_snapshot();
      result.posted_count = win.in_window_count() + win.warmup_done_count();
    }
    // Duration mode: 1-byte sentinel so the echo server stops (it cannot know
    // the round-trip count). The server recv completes with n==1 (!= size) and
    // breaks without echoing.
    if (win.duration_mode()) {
      storage[0] = 'E';
      qp.async_send(mr.cslice(std::size_t{0}, std::size_t{1}), use_fut).get();
    }

    rdma_bench::finalize_counts(result, win);
    finish_throughput(result, win.window_begin_wall(), clock_type::now());
    rdma_bench::fill_cpu_metrics(result, cpu_begin,
                                 rdma_bench::take_cpu_snapshot());
    rdma_bench::fill_latency_cycles(result, win.lat_deltas(), 2);
    result.validation_passed = true;
    asio::error_code ignored;
    conn.disconnect(ignored);
    return result;
  } catch (std::exception const& e) {
    return failed_poll_result(opt, command_line, e.what());
  }
}

int run_poll_bandwidth(rdma_bench::options opt, std::string command_line) {
  if (opt.token_type != "callback" && opt.token_type != "use_future") {
    auto r = rdma_bench::make_skip_result(
        opt, command_line,
        "poll-mode send/recv bandwidth supports callback and as_tuple(use_future)",
        "poll_token_" + opt.token_type);
    rdma_bench::write_result(r, opt.json_out);
    return 0;
  }

  rdma_bench::result selected;
  if (opt.single_process) {
    std::promise<void> ready;
    auto ready_fut = ready.get_future();
    auto server_opt = opt;
    server_opt.topology = "single_host_same_process";
    auto client_opt = opt;
    client_opt.peer_addr = opt.local_addr;
    client_opt.topology = "single_host_same_process";
    rdma_bench::result server_result;
    std::thread server([&] {
      if (opt.token_type == "callback") {
        server_result = run_poll_callback_bandwidth_server_role(
            server_opt, command_line, &ready);
      } else {
        server_result =
            run_poll_bandwidth_server_role(server_opt, command_line, &ready);
      }
    });
    ready_fut.wait();
    if (opt.token_type == "callback") {
      selected =
          run_poll_callback_bandwidth_client_role(client_opt, command_line);
    } else {
      selected = run_poll_bandwidth_client_role(client_opt, command_line);
    }
    if (server.joinable()) server.join();
    if (selected.errors != 0 && server_result.errors != 0) {
      selected.first_error += "; server: " + server_result.first_error;
    }
  } else if (opt.server) {
    if (opt.token_type == "callback") {
      selected =
          run_poll_callback_bandwidth_server_role(opt, command_line, nullptr);
    } else {
      selected = run_poll_bandwidth_server_role(opt, command_line, nullptr);
    }
  } else {
    if (opt.token_type == "callback") {
      selected = run_poll_callback_bandwidth_client_role(opt, command_line);
    } else {
      selected = run_poll_bandwidth_client_role(opt, command_line);
    }
  }

  rdma_bench::write_result(selected, opt.json_out);
  return selected.exit_code;
}

int run_poll_latency(rdma_bench::options opt, std::string command_line) {
  if (opt.token_type != "use_future") {
    auto r = rdma_bench::make_skip_result(
        opt, command_line,
        "poll-mode send/recv latency uses as_tuple(use_future) in this benchmark",
        "poll_token_" + opt.token_type);
    rdma_bench::write_result(r, opt.json_out);
    return 0;
  }

  rdma_bench::result selected;
  if (opt.single_process) {
    std::promise<void> ready;
    auto ready_fut = ready.get_future();
    auto server_opt = opt;
    server_opt.topology = "single_host_same_process";
    auto client_opt = opt;
    client_opt.peer_addr = opt.local_addr;
    client_opt.topology = "single_host_same_process";
    rdma_bench::result server_result;
    std::thread server([&] {
      server_result =
          run_poll_latency_server_role(server_opt, command_line, &ready);
    });
    ready_fut.wait();
    selected = run_poll_latency_client_role(client_opt, command_line);
    if (server.joinable()) server.join();
    if (selected.errors != 0 && server_result.errors != 0) {
      selected.first_error += "; server: " + server_result.first_error;
    }
  } else if (opt.server) {
    selected = run_poll_latency_server_role(opt, command_line, nullptr);
  } else {
    selected = run_poll_latency_client_role(opt, command_line);
  }

  rdma_bench::write_result(selected, opt.json_out);
  return selected.exit_code;
}

}  // anonymous namespace

namespace asio_perftest {

// send/recv dispatch (mode x metric). Operation routing + option parsing live in
// the unified asio_perftest main; this entry assumes operation == send_recv.
int run_send_recv(rdma_bench::options opt, std::string cmd) {
  if (opt.mode == "poll") {
    if (opt.metric == rdma_bench::metric_kind::latency) {
      return run_poll_latency(std::move(opt), std::move(cmd));
    }
    return run_poll_bandwidth(std::move(opt), std::move(cmd));
  }
  if (opt.metric == rdma_bench::metric_kind::latency) {
    return run_event_latency(std::move(opt), std::move(cmd));
  }
  return run_event_bandwidth(std::move(opt), std::move(cmd));
}

}  // namespace asio_perftest
