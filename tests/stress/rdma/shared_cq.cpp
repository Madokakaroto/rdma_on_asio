#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asio/buffer.hpp"
#include "asio/io_context.hpp"
#include "asio/ip/address.hpp"
#include "asio/steady_timer.hpp"

#include "rdma/rdma.hpp"
#include "rdma_bench_common.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

// Shared-CQ multi-QP stress. Each QP runs a request/echo ping-pong, but with a
// PRE-POSTED RECEIVE WINDOW instead of a strict 1-deep co_await loop: the server
// keeps `window` receives posted at all times (re-posting one as each echo
// completes), and the client posts the echo receive before sending. A receive is
// therefore always waiting before any send arrives, so the data path never hits
// the RNR stall that the strict ping-pong did on single-host loopback. All
// callbacks run on the shared io_context CQ (multi-thread run()), so the shared
// counters are atomic.

namespace {

std::size_t pick_window(rdma_bench::options const& opt) {
  // window >= 2 keeps at least one receive posted while one is being echoed;
  // never post more than the per-QP message budget.
  std::uint64_t w = std::max<std::uint32_t>(2, opt.queue_depth);
  return static_cast<std::size_t>(std::min<std::uint64_t>(w, opt.iterations));
}

}  // namespace

// Server side: windowed echo. Keeps `window_` receives posted; echoes each.
class echo_server : public std::enable_shared_from_this<echo_server> {
public:
  echo_server(asio::io_context& io, rdma::rdma_device_ptr device,
              rdma::rdma_listener<tcp>& listener, rdma_bench::options const& opt,
              std::atomic<std::uint64_t>& completed,
              std::atomic<std::uint64_t>& errors, std::function<void()> done)
      : device_(std::move(device))
      , listener_(listener)
      , conn_(io)
      , qp_(io)
      , msg_(opt.message_size)
      , window_(pick_window(opt))
      , target_(opt.iterations)
      , completed_(completed)
      , errors_(errors)
      , done_(std::move(done)) {}

  void start() {
    auto self = shared_from_this();
    listener_.async_get_connection(
        asio::mutable_buffer{},
        [self](asio::error_code ec, rdma::rdma_connector<tcp> conn,
               std::size_t) {
          if (ec) { self->fail(); return; }
          self->conn_ = std::move(conn);
          self->conn_.async_accept(self->qp_, asio::const_buffer{},
                                   [self](asio::error_code ec) {
                                     if (ec) { self->fail(); return; }
                                     self->begin();
                                   });
        });
  }

private:
  void fail() {
    if (!finished_.exchange(true)) {
      ++errors_;
      done_();
    }
  }
  void finish_ok() {
    if (!finished_.exchange(true)) done_();
  }

  void begin() {
    storage_.assign(msg_ * window_, 0);
    mr_ = std::make_unique<rdma::rdma_memory_region>(device_, storage_.data(),
                                                     storage_.size());
    recvs_posted_.store(window_);
    for (std::size_t slot = 0; slot < window_; ++slot) post_recv(slot);
  }

  void post_recv(std::size_t slot) {
    auto self = shared_from_this();
    qp_.async_recv(mr_->slice(slot * msg_, msg_),
                   [self, slot](asio::error_code ec, std::size_t n) {
                     self->on_recv(slot, ec, n);
                   });
  }

  void on_recv(std::size_t slot, asio::error_code ec, std::size_t n) {
    if (finished_.load(std::memory_order_acquire)) return;
    if (ec || n != msg_) { fail(); return; }
    auto self = shared_from_this();
    qp_.async_send(mr_->cslice(slot * msg_, n),
                   [self, slot](asio::error_code ec, std::size_t sn) {
                     self->on_send(slot, ec, sn);
                   });
  }

  void on_send(std::size_t slot, asio::error_code ec, std::size_t sn) {
    if (finished_.load(std::memory_order_acquire)) return;
    if (ec || sn != msg_) { fail(); return; }
    std::uint64_t const e = echoed_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (e > target_) return;  // budget already met by another slot
    ++completed_;
    if (e == target_) { finish_ok(); return; }
    // Maintain the window: re-post this slot's receive, capped at the budget.
    if (recvs_posted_.fetch_add(1, std::memory_order_acq_rel) + 1 <= target_) {
      post_recv(slot);
    }
  }

  rdma::rdma_device_ptr device_;
  rdma::rdma_listener<tcp>& listener_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  std::size_t msg_;
  std::size_t window_;
  std::uint64_t target_;
  std::atomic<std::uint64_t>& completed_;
  std::atomic<std::uint64_t>& errors_;
  std::function<void()> done_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  std::atomic<std::uint64_t> echoed_{0};
  std::atomic<std::uint64_t> recvs_posted_{0};
  std::atomic<bool> finished_{false};
};

// Client side: strict request/echo, but posts the echo receive before each send.
class pingpong_client : public std::enable_shared_from_this<pingpong_client> {
public:
  pingpong_client(asio::io_context& io, rdma::rdma_device_ptr device,
                  rdma_bench::options const& opt, std::uint16_t port,
                  std::atomic<std::uint64_t>& completed,
                  std::atomic<std::uint64_t>& errors, std::function<void()> done)
      : device_(std::move(device))
      , conn_(io)
      , qp_(io)
      , addr_(opt.local_addr)
      , port_(port)
      , msg_(opt.message_size)
      , target_(opt.iterations)
      , completed_(completed)
      , errors_(errors)
      , done_(std::move(done)) {}

  void start() {
    conn_.open(rdma_test::port_space_for(addr_));
    auto self = shared_from_this();
    auto ep = rdma_test::endpoint_for(addr_, port_);
    conn_.async_connect(qp_, ep, asio::const_buffer{},
                        [self](asio::error_code ec) {
                          if (ec) { self->fail(); return; }
                          self->begin();
                        });
  }

private:
  void fail() {
    if (!finished_.exchange(true)) {
      ++errors_;
      done_();
    }
  }
  void finish_ok() {
    if (!finished_.exchange(true)) done_();
  }

  void begin() {
    storage_.assign(msg_ * 2, 0);  // [0,msg)=recv echo, [msg,2msg)=send source
    for (std::size_t i = msg_; i < storage_.size(); ++i) {
      storage_[i] = static_cast<char>((i * 13) & 0x7f);
    }
    mr_ = std::make_unique<rdma::rdma_memory_region>(device_, storage_.data(),
                                                     storage_.size());
    round();
  }

  void round() {
    if (finished_.load(std::memory_order_acquire)) return;
    auto self = shared_from_this();
    // Post the echo receive BEFORE the send so the server's reply always finds a
    // posted receive (no RNR on the echo direction).
    qp_.async_recv(mr_->slice(std::size_t{0}, msg_),
                   [self](asio::error_code ec, std::size_t n) {
                     self->on_recv(ec, n);
                   });
    qp_.async_send(mr_->cslice(msg_, msg_),
                   [self](asio::error_code ec, std::size_t sn) {
                     self->on_send(ec, sn);
                   });
  }

  void on_send(asio::error_code ec, std::size_t sn) {
    if (finished_.load(std::memory_order_acquire)) return;
    if (ec || sn != msg_) fail();
  }

  void on_recv(asio::error_code ec, std::size_t n) {
    if (finished_.load(std::memory_order_acquire)) return;
    if (ec || n != msg_) { fail(); return; }
    ++completed_;
    if (++received_ >= target_) { finish_ok(); return; }
    round();
  }

  rdma::rdma_device_ptr device_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  std::string addr_;
  std::uint16_t port_;
  std::size_t msg_;
  std::uint64_t target_;
  std::atomic<std::uint64_t>& completed_;
  std::atomic<std::uint64_t>& errors_;
  std::function<void()> done_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  std::uint64_t received_{0};
  std::atomic<bool> finished_{false};
};

int main(int argc, char* argv[]) {
  try {
    auto opt = rdma_bench::parse_options_with_scenario(argc, argv, false);
    auto cmd = rdma_bench::command_line(argc, argv);
    opt.local_addr =
        rdma_test::local_device_address_string(asio::rdma::tcp::v4());
    if (opt.mode != "event") {
      auto r = rdma_bench::make_skip_result(
          opt, cmd, "shared-CQ stress targets event-mode QPs",
          "stress_shared_cq_" + opt.mode);
      rdma_bench::write_result(r, opt.json_out);
      return 0;
    }

    asio::io_context io;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device({});
    rdma::use_device(io, device);

    // Stage 4 contract: an io_context that binds no event-mode QP must never
    // start the shared-CQ poller, so its run() returns on idle. A poller wrongly
    // started (e.g. eagerly at use_device) would block run() forever -- detect it
    // on a throwaway control-plane-only io_context before the main stress.
    bool idle_return_ok = true;
    {
      asio::io_context idle_io;
      rdma::use_device(idle_io, device);
      rdma::rdma_listener<tcp> idle_listener(idle_io);
      idle_listener.open(rdma_test::port_space_for(opt.local_addr));
      std::atomic<bool> returned{false};
      std::thread idle_thread([&] {
        idle_io.run();
        returned.store(true);
      });
      for (int i = 0; i < 200 && !returned.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!returned.load()) {
        idle_return_ok = false;  // run() blocked -> poller contract broken
        idle_io.stop();
      }
      idle_thread.join();
    }

    std::vector<std::unique_ptr<rdma::rdma_listener<tcp>>> listeners;
    listeners.reserve(opt.qps);
    for (std::uint32_t i = 0; i < opt.qps; ++i) {
      auto listener = std::make_unique<rdma::rdma_listener<tcp>>(io);
      listener->open(rdma_test::port_space_for(opt.local_addr));
      listener->bind(static_cast<std::uint16_t>(opt.port + i));
      listener->listen();
      listeners.push_back(std::move(listener));
    }

    std::atomic<std::uint64_t> server_completed{0};
    std::atomic<std::uint64_t> client_completed{0};
    std::atomic<std::uint64_t> errors{0};
    std::atomic<int> remaining{static_cast<int>(opt.qps * 2)};
    auto on_done = [&] {
      if (--remaining == 0) io.stop();
    };

    // Keep the role objects alive for the whole run.
    std::vector<std::shared_ptr<echo_server>> servers;
    std::vector<std::shared_ptr<pingpong_client>> clients;
    servers.reserve(opt.qps);
    clients.reserve(opt.qps);
    for (std::uint32_t i = 0; i < opt.qps; ++i) {
      auto srv = std::make_shared<echo_server>(io, device, *listeners[i], opt,
                                               server_completed, errors,
                                               on_done);
      auto cli = std::make_shared<pingpong_client>(
          io, device, opt, static_cast<std::uint16_t>(opt.port + i),
          client_completed, errors, on_done);
      servers.push_back(srv);
      clients.push_back(cli);
      srv->start();
      cli->start();
    }

    asio::steady_timer watchdog(io);
    watchdog.expires_after(std::chrono::seconds(opt.timeout_sec));
    watchdog.async_wait([&](asio::error_code ec) {
      if (!ec) {
        ++errors;
        io.stop();
      }
    });

    std::vector<std::thread> threads;
    auto thread_count = std::max<std::uint32_t>(1, opt.threads);
    threads.reserve(thread_count);
    for (std::uint32_t i = 0; i < thread_count; ++i) {
      threads.emplace_back([&] { io.run(); });
    }
    for (auto& t : threads) t.join();
    watchdog.cancel();

    auto r = rdma_bench::make_base_result(opt, cmd);
    r.scenario_name = "shared_cq_stress";
    r.posted_count = opt.qps * opt.iterations * 2;
    r.completed_count = server_completed.load() + client_completed.load();
    r.payload_bytes = r.completed_count * opt.message_size;
    r.errors = errors.load();
    r.validation_passed = (r.errors == 0 &&
                           r.completed_count == r.posted_count &&
                           idle_return_ok);
    if (!*r.validation_passed) {
      r.first_error =
          !idle_return_ok
              ? "poll/control-only io_context run() did not return on idle "
                "(shared-CQ poller contract broken)"
              : "shared-CQ stress completion count mismatch or error";
      r.exit_code = 1;
    }
    rdma_bench::write_result(r, opt.json_out);
    return r.exit_code;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
