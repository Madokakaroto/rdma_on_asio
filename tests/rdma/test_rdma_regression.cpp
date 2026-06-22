// Cross-platform hardware regression tests for the backend-agnostic rdma_* API.
//
// These are correctness regressions, not stress or performance tests:
//   - RDMA read/write round trip against a peer memory region.
//   - Zero-length send/recv immediate completion on an established QP.
//   - Multi-message send/recv ordering.
//   - Negative connect to a port with no listener.
//
// Usage: test_rdma_regression <roce-ip> [port]   (skips if no arg).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/buffer.hpp"
#include "asio/co_spawn.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
using namespace std::chrono_literals;

constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

struct phase_guard {
  asio::steady_timer watchdog;
  std::atomic<int> remaining;
  bool timed_out = false;
  std::exception_ptr exception;

  phase_guard(asio::io_context& io, int ops, std::chrono::seconds timeout)
      : watchdog(io)
      , remaining(ops) {
    watchdog.expires_after(timeout);
    watchdog.async_wait([this, &io](asio::error_code ec) {
      if (!ec) {
        timed_out = true;
        io.stop();
      }
    });
  }

  auto on_done(asio::io_context& io) {
    return [this, &io](std::exception_ptr e) {
      if (e && !exception) {
        exception = e;
      }
      if (--remaining == 0) {
        watchdog.cancel();
        io.stop();
      }
    };
  }
};

bool finish_phase(char const* name, phase_guard& guard, bool ok) {
  if (guard.exception) {
    try {
      std::rethrow_exception(guard.exception);
    } catch (std::exception const& e) {
      std::cerr << "[FAIL] " << name << ": exception: " << e.what() << "\n";
    }
    return false;
  }
  if (guard.timed_out) {
    std::cerr << "[FAIL] " << name << ": watchdog timeout\n";
    return false;
  }
  if (ok) {
    std::cout << "[PASS] " << name << "\n";
  } else {
    std::cerr << "[FAIL] " << name << "\n";
  }
  return ok;
}

bool phase_open_guards(rdma::rdma_device_ptr const& device) {
  char const* const name = "connector/listener duplicate open guards";
  asio::io_context io;
  rdma::use_device(io, device);

  asio::error_code ec;
  rdma::rdma_connector<tcp> connector(io);
  connector.open(tcp::v4(), ec);
  bool const connector_opened = !ec && connector.is_open();
  connector.open(tcp::v4(), ec);
  bool const connector_duplicate = (ec == asio::error::already_open);

  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4(), ec);
  bool const listener_opened = !ec && listener.is_open();
  listener.open(tcp::v4(), ec);
  bool const listener_duplicate = (ec == asio::error::already_open);

  bool const ok = connector_opened && connector_duplicate && listener_opened &&
                  listener_duplicate;
  if (ok) {
    std::cout << "[PASS] " << name << "\n";
  } else {
    std::cerr << "[FAIL] " << name
              << ": connector_opened=" << connector_opened
              << " connector_duplicate=" << connector_duplicate
              << " listener_opened=" << listener_opened
              << " listener_duplicate=" << listener_duplicate << "\n";
  }
  return ok;
}

bool phase_queue_pair_and_cq_guards(rdma::rdma_device_ptr const& device) {
  char const* const name = "queue-pair bind modes and empty CQ poll";
  asio::io_context io;
  rdma::use_device(io, device);

  asio::error_code ec;
  rdma::rdma_queue_pair event_qp;
  bool const default_ok =
      !event_qp.is_bound() &&
      event_qp.bound_type() == rdma::completion_mode::none;

  event_qp.bind(io, ec);
  bool const event_ok = !ec && event_qp.is_bound() &&
                        event_qp.bound_type() == rdma::completion_mode::event;
  event_qp.bind(io, ec);
  bool const event_duplicate = (ec == asio::error::already_open);

  rdma::rdma_completion_queue cq(device);
  bool const empty_poll_ok = (cq.poll_one() == 0) && (cq.poll() == 0);

  rdma::rdma_queue_pair poll_qp;
  poll_qp.bind(cq, ec);
  bool const poll_ok = !ec && poll_qp.is_bound() &&
                       poll_qp.bound_type() == rdma::completion_mode::poll;
  poll_qp.bind(cq, ec);
  bool const poll_duplicate = (ec == asio::error::already_open);

  bool const ok = default_ok && event_ok && event_duplicate && empty_poll_ok &&
                  poll_ok && poll_duplicate;
  if (ok) {
    std::cout << "[PASS] " << name << "\n";
  } else {
    std::cerr << "[FAIL] " << name << ": default=" << default_ok
              << " event=" << event_ok
              << " event_dup=" << event_duplicate
              << " empty_poll=" << empty_poll_ok
              << " poll=" << poll_ok
              << " poll_dup=" << poll_duplicate << "\n";
  }
  return ok;
}

bool phase_memory_region_boundaries(rdma::rdma_device_ptr const& device) {
  char const* const name = "memory-region boundaries and moved-from guard";
  std::array<unsigned char, 64> storage{};
  rdma::rdma_memory_region mr(device, storage.data(), storage.size());

  auto const local_key = mr.local_key();
  bool const metadata_ok = mr.addr() == storage.data() &&
                           mr.length() == storage.size();
  bool const range_ok =
      mr.is_in_mr(std::size_t{0}, std::size_t{0}) &&
      mr.is_in_mr(std::size_t{0}, std::size_t{1}) &&
      mr.is_in_mr(std::size_t{8}, std::size_t{16}) &&
      mr.is_in_mr(storage.size(), std::size_t{0}) &&
      !mr.is_in_mr(storage.size(), std::size_t{1}) &&
      !mr.is_in_mr(storage.size() - 1, std::size_t{2});

  auto mutable_slice = mr.slice(std::size_t{4}, std::size_t{8});
  auto const_slice = mr.cslice(std::size_t{12}, std::size_t{8});
  auto invalid_slice = mr.slice(storage.size() + 1, std::size_t{1});
  auto remote = mr.remote_addr(std::size_t{4}, std::size_t{8});
  auto invalid_remote = mr.remote_addr(storage.size() + 1, std::size_t{1});

  bool const slice_ok =
      mutable_slice.addr() == storage.data() + 4 &&
      mutable_slice.length() == 8 &&
      mutable_slice.local_key() == local_key &&
      const_slice.addr() == storage.data() + 12 &&
      const_slice.length() == 8 &&
      invalid_slice.addr() == nullptr &&
      invalid_slice.length() == 0 &&
      remote.addr_ == reinterpret_cast<std::uint64_t>(storage.data() + 4) &&
      remote.token_ == mr.remote_key() &&
      invalid_remote.addr_ == 0 &&
      invalid_remote.token_ == 0;

  rdma::rdma_memory_region moved(std::move(mr));
  bool moved_from_local_key_rejected = false;
  bool moved_from_remote_key_rejected = false;
  try {
    (void)mr.local_key();
  } catch (std::system_error const& e) {
    moved_from_local_key_rejected =
        (e.code() == rdma::rdma_errc::invalid_handle);
  }
  try {
    (void)mr.remote_key();
  } catch (std::system_error const& e) {
    moved_from_remote_key_rejected =
        (e.code() == rdma::rdma_errc::invalid_handle);
  }

  (void)moved.local_key();
  bool const moved_ok =
      moved_from_local_key_rejected && moved_from_remote_key_rejected;

  bool const ok = metadata_ok && range_ok && slice_ok && moved_ok;
  if (ok) {
    std::cout << "[PASS] " << name << "\n";
  } else {
    std::cerr << "[FAIL] " << name << ": metadata=" << metadata_ok
              << " range=" << range_ok
              << " slice=" << slice_ok
              << " moved=" << moved_ok << "\n";
  }
  return ok;
}

bool phase_read_write(rdma::rdma_device_ptr const& device,
                      std::string const& ip, std::uint16_t port) {
  char const* const name = "read/write round trip";
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(port);
  listener.listen();

  constexpr std::string_view readable = "server-readable-payload";
  constexpr std::string_view write_payload = "client-wrote-this";
  constexpr std::size_t write_offset = 96;

  std::array<char, 256> server_storage{};
  std::memcpy(server_storage.data(), readable.data(), readable.size());
  rdma::rdma_memory_region server_mr(device, server_storage.data(),
                                     server_storage.size());
  auto const read_remote = server_mr.remote_addr(std::size_t{0},
                                                 readable.size());
  auto const write_remote =
      server_mr.remote_addr(write_offset, write_payload.size());

  bool read_ok = false;
  bool write_ok = false;
  phase_guard guard(io, 2, 10s);
  auto done = guard.on_done(io);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ec_get, conn, req_len] =
            co_await listener.async_get_connection(asio::mutable_buffer{},
                                                   nothrow);
        (void)req_len;
        if (ec_get) {
          std::cerr << "[read/write server] get_connection: "
                    << ec_get.message() << "\n";
          co_return;
        }

        rdma::rdma_queue_pair qp(io);
        auto [ec_accept] =
            co_await conn.async_accept(qp, asio::const_buffer{}, nothrow);
        if (ec_accept) {
          std::cerr << "[read/write server] accept: "
                    << ec_accept.message() << "\n";
          co_return;
        }

        std::array<char, 1> control{};
        rdma::rdma_memory_region control_mr(device, control.data(),
                                            control.size());
        auto [ec_recv, n] =
            co_await qp.async_recv(
                control_mr.slice(std::size_t{0}, control.size()), nothrow);
        write_ok = !ec_recv && n == 1 &&
                   std::string_view(server_storage.data() + write_offset,
                                    write_payload.size()) == write_payload;
        if (!write_ok) {
          std::cerr << "[read/write server] write verification failed: ec="
                    << ec_recv.message() << " n=" << n << "\n";
        }
        co_await conn.async_wait_disconnect(nothrow);
      },
      done);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(tcp::v4());
        rdma::rdma_queue_pair qp(io);
        tcp::endpoint endpoint(asio::ip::make_address(ip), port);
        auto [ec_connect, reply_len] = co_await conn.async_connect(
            qp, endpoint, asio::const_buffer{}, asio::mutable_buffer{},
            nothrow);
        (void)reply_len;
        if (ec_connect) {
          std::cerr << "[read/write client] connect: "
                    << ec_connect.message() << "\n";
          co_return;
        }

        std::array<char, 256> client_storage{};
        rdma::rdma_memory_region client_mr(device, client_storage.data(),
                                           client_storage.size());
        auto [ec_read, read_n] = co_await qp.async_read(
            client_mr.slice(std::size_t{0}, readable.size()), read_remote,
            nothrow);
        read_ok = !ec_read && read_n == readable.size() &&
                  std::string_view(client_storage.data(), readable.size()) ==
                      readable;
        if (!read_ok) {
          std::cerr << "[read/write client] read verification failed: ec="
                    << ec_read.message() << " n=" << read_n << "\n";
          conn.disconnect();
          co_return;
        }

        std::memcpy(client_storage.data() + 128, write_payload.data(),
                    write_payload.size());
        auto [ec_write, write_n] = co_await qp.async_write(
            client_mr.cslice(std::size_t{128}, write_payload.size()),
            write_remote, nothrow);
        if (ec_write || write_n != write_payload.size()) {
          std::cerr << "[read/write client] write failed: ec="
                    << ec_write.message() << " n=" << write_n << "\n";
          conn.disconnect();
          co_return;
        }

        client_storage[240] = '!';
        auto [ec_send, send_n] =
            co_await qp.async_send(
                client_mr.cslice(std::size_t{240}, std::size_t{1}), nothrow);
        if (ec_send || send_n != 1) {
          std::cerr << "[read/write client] control send failed: ec="
                    << ec_send.message() << " n=" << send_n << "\n";
        }
        conn.disconnect();
      },
      done);

  io.run();
  return finish_phase(name, guard, read_ok && write_ok);
}

bool phase_zero_length(rdma::rdma_device_ptr const& device,
                       std::string const& ip, std::uint16_t port) {
  char const* const name = "zero-length send/recv immediate completion";
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(port);
  listener.listen();

  bool server_zero_ok = false;
  bool client_zero_ok = false;
  bool alive_after = false;
  phase_guard guard(io, 2, 10s);
  auto done = guard.on_done(io);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ec_get, conn, req_len] =
            co_await listener.async_get_connection(asio::mutable_buffer{},
                                                   nothrow);
        (void)req_len;
        if (ec_get) co_return;

        rdma::rdma_queue_pair qp(io);
        auto [ec_accept] =
            co_await conn.async_accept(qp, asio::const_buffer{}, nothrow);
        if (ec_accept) co_return;

        auto [ec_zrecv, zrecv_n] =
            co_await qp.async_recv(rdma::mutable_buffer{}, nothrow);
        auto [ec_zsend, zsend_n] =
            co_await qp.async_send(rdma::const_buffer{}, nothrow);
        server_zero_ok = !ec_zrecv && !ec_zsend && zrecv_n == 0 &&
                         zsend_n == 0;

        std::array<char, 32> storage{};
        rdma::rdma_memory_region mr(device, storage.data(), storage.size());
        auto [ec_recv, n] =
            co_await qp.async_recv(
                mr.slice(std::size_t{0}, storage.size()), nothrow);
        if (!ec_recv && std::string_view(storage.data(), n) == "alive") {
          auto [ec_send, send_n] =
              co_await qp.async_send(mr.cslice(std::size_t{0}, n), nothrow);
          (void)send_n;
          alive_after = !ec_send;
        }
        co_await conn.async_wait_disconnect(nothrow);
      },
      done);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(tcp::v4());
        rdma::rdma_queue_pair qp(io);
        tcp::endpoint endpoint(asio::ip::make_address(ip), port);
        auto [ec_connect, reply_len] = co_await conn.async_connect(
            qp, endpoint, asio::const_buffer{}, asio::mutable_buffer{},
            nothrow);
        (void)reply_len;
        if (ec_connect) co_return;

        auto [ec_zsend, zsend_n] =
            co_await qp.async_send(rdma::const_buffer{}, nothrow);
        auto [ec_zrecv, zrecv_n] =
            co_await qp.async_recv(rdma::mutable_buffer{}, nothrow);
        client_zero_ok = !ec_zsend && !ec_zrecv && zsend_n == 0 &&
                         zrecv_n == 0;

        std::array<char, 32> storage{};
        std::memcpy(storage.data(), "alive", 5);
        rdma::rdma_memory_region mr(device, storage.data(), storage.size());
        auto [ec_send, send_n] =
            co_await qp.async_send(
                mr.cslice(std::size_t{0}, std::size_t{5}), nothrow);
        if (!ec_send && send_n == 5) {
          auto [ec_recv, recv_n] =
              co_await qp.async_recv(
                  mr.slice(std::size_t{0}, storage.size()), nothrow);
          alive_after = alive_after && !ec_recv && recv_n == 5 &&
                        std::string_view(storage.data(), recv_n) == "alive";
        }
        conn.disconnect();
      },
      done);

  io.run();
  return finish_phase(name, guard,
                      server_zero_ok && client_zero_ok && alive_after);
}

namespace {

constexpr std::size_t mm_slot_bytes = 64;

// Windowed echo server: keeps `window` receives posted (re-posting as each echo
// completes) so a receive is always waiting before the client's next send -- no
// RNR. The client here is strict 1-in-flight, so receive completions arrive in
// send order and `recv_seq_` validates ordering. Single io_context thread, so
// plain (non-atomic) counters are fine.
class mm_echo_server : public std::enable_shared_from_this<mm_echo_server> {
public:
  mm_echo_server(asio::io_context& io, rdma::rdma_device_ptr device,
                 rdma::rdma_listener<tcp>& listener, int count, bool& ok,
                 std::function<void()> done)
      : device_(std::move(device))
      , listener_(listener)
      , conn_(io)
      , qp_(io)
      , count_(count)
      , window_(std::min(8, count))
      , ok_(ok)
      , done_(std::move(done)) {}

  void start() {
    auto self = shared_from_this();
    listener_.async_get_connection(
        asio::mutable_buffer{},
        [self](asio::error_code ec, rdma::rdma_connector<tcp> conn,
               std::size_t) {
          if (ec) { self->finish(); return; }
          self->conn_ = std::move(conn);
          self->conn_.async_accept(self->qp_, asio::const_buffer{},
                                   [self](asio::error_code ec) {
                                     if (ec) { self->finish(); return; }
                                     self->begin();
                                   });
        });
  }

private:
  void finish() {
    if (!done_called_) { done_called_ = true; done_(); }
  }

  void begin() {
    storage_.assign(mm_slot_bytes * static_cast<std::size_t>(window_), 0);
    mr_ = std::make_unique<rdma::rdma_memory_region>(device_, storage_.data(),
                                                     storage_.size());
    recvs_posted_ = window_;
    for (int slot = 0; slot < window_; ++slot) post_recv(slot);
  }

  void post_recv(int slot) {
    auto self = shared_from_this();
    qp_.async_recv(
        mr_->slice(static_cast<std::size_t>(slot) * mm_slot_bytes, mm_slot_bytes),
        [self, slot](asio::error_code ec, std::size_t n) {
          self->on_recv(slot, ec, n);
        });
  }

  void on_recv(int slot, asio::error_code ec, std::size_t n) {
    if (done_called_) return;
    auto const off = static_cast<std::size_t>(slot) * mm_slot_bytes;
    std::string expected = "msg-" + std::to_string(recv_seq_);
    if (ec || std::string_view(storage_.data() + off, n) != expected) {
      std::cerr << "[ordering server] expected " << expected
                << " got ec=" << ec.message() << " n=" << n << "\n";
      ok_ = false;
      finish();
      return;
    }
    ++recv_seq_;
    auto self = shared_from_this();
    qp_.async_send(mr_->cslice(off, n),
                   [self, slot](asio::error_code ec, std::size_t sn) {
                     self->on_send(slot, ec, sn);
                   });
  }

  void on_send(int slot, asio::error_code ec, std::size_t /*sn*/) {
    if (done_called_) return;
    if (ec) { ok_ = false; finish(); return; }
    if (++echoed_ >= count_) { finish(); return; }
    if (recvs_posted_ < count_) {
      ++recvs_posted_;
      post_recv(slot);
    }
  }

  rdma::rdma_device_ptr device_;
  rdma::rdma_listener<tcp>& listener_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  int count_;
  int window_;
  bool& ok_;
  std::function<void()> done_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  int recv_seq_ = 0;
  int echoed_ = 0;
  int recvs_posted_ = 0;
  bool done_called_ = false;
};

// Strict request/echo client: posts the echo receive BEFORE each send, so the
// server's reply always finds a posted receive (no RNR on the echo direction).
class mm_client : public std::enable_shared_from_this<mm_client> {
public:
  mm_client(asio::io_context& io, rdma::rdma_device_ptr device,
            std::string ip, std::uint16_t port, int count, bool& ok,
            std::function<void()> done)
      : device_(std::move(device))
      , conn_(io)
      , qp_(io)
      , ip_(std::move(ip))
      , port_(port)
      , count_(count)
      , ok_(ok)
      , done_(std::move(done)) {}

  void start() {
    conn_.open(tcp::v4());
    auto self = shared_from_this();
    tcp::endpoint ep(asio::ip::make_address(ip_), port_);
    conn_.async_connect(qp_, ep, asio::const_buffer{},
                        [self](asio::error_code ec) {
                          if (ec) { self->finish(); return; }
                          self->begin();
                        });
  }

private:
  void finish() {
    if (!done_called_) { done_called_ = true; done_(); }
  }

  void begin() {
    storage_.assign(mm_slot_bytes * 2, 0);  // [0,64)=recv echo, [64,128)=send
    mr_ = std::make_unique<rdma::rdma_memory_region>(device_, storage_.data(),
                                                     storage_.size());
    round();
  }

  void round() {
    if (done_called_) return;
    std::string msg = "msg-" + std::to_string(sent_);
    std::memset(storage_.data() + mm_slot_bytes, 0, mm_slot_bytes);
    std::memcpy(storage_.data() + mm_slot_bytes, msg.data(), msg.size());
    cur_len_ = msg.size();
    auto self = shared_from_this();
    qp_.async_recv(mr_->slice(std::size_t{0}, mm_slot_bytes),
                   [self](asio::error_code ec, std::size_t n) {
                     self->on_recv(ec, n);
                   });
    qp_.async_send(mr_->cslice(mm_slot_bytes, cur_len_),
                   [self](asio::error_code ec, std::size_t sn) {
                     self->on_send(ec, sn);
                   });
    ++sent_;
  }

  void on_send(asio::error_code ec, std::size_t sn) {
    if (done_called_) return;
    if (ec || sn != cur_len_) { ok_ = false; finish(); }
  }

  void on_recv(asio::error_code ec, std::size_t n) {
    if (done_called_) return;
    std::string expected = "msg-" + std::to_string(received_);
    if (ec || std::string_view(storage_.data(), n) != expected) {
      ok_ = false;
      finish();
      return;
    }
    if (++received_ >= count_) {
      conn_.disconnect();
      finish();
      return;
    }
    round();
  }

  rdma::rdma_device_ptr device_;
  rdma::rdma_connector<tcp> conn_;
  rdma::rdma_queue_pair qp_;
  std::string ip_;
  std::uint16_t port_;
  int count_;
  bool& ok_;
  std::function<void()> done_;
  std::vector<char> storage_;
  std::unique_ptr<rdma::rdma_memory_region> mr_;
  int sent_ = 0;
  int received_ = 0;
  std::size_t cur_len_ = 0;
  bool done_called_ = false;
};

}  // namespace

bool phase_multi_message_order(rdma::rdma_device_ptr const& device,
                               std::string const& ip, std::uint16_t port) {
  char const* const name = "multi-message ordering";
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(port);
  listener.listen();

  constexpr int message_count = 16;
  bool server_order_ok = true;
  bool client_echo_ok = true;
  phase_guard guard(io, 2, 10s);
  auto gd = guard.on_done(io);
  auto role_done = [gd]() mutable { gd(std::exception_ptr{}); };

  auto server = std::make_shared<mm_echo_server>(
      io, device, listener, message_count, server_order_ok, role_done);
  auto client = std::make_shared<mm_client>(io, device, ip, port,
                                            message_count, client_echo_ok,
                                            role_done);
  server->start();
  client->start();

  io.run();
  return finish_phase(name, guard, server_order_ok && client_echo_ok);
}

bool phase_negative_connect(rdma::rdma_device_ptr const& device,
                            std::string const& ip, std::uint16_t port) {
  char const* const name = "negative connect without listener";
  asio::io_context io;
  rdma::rdma_config_t config{};
  config.cm_resolve_timeout_ms_ = 250;
  rdma::use_device(io, device, config);

  bool completed = false;
  asio::error_code got;
  asio::steady_timer watchdog(io);
  watchdog.expires_after(10s);
  watchdog.async_wait([&](asio::error_code ec) {
    if (!ec) {
      io.stop();
    }
  });

  rdma::rdma_connector<tcp> conn(io);
  rdma::rdma_queue_pair qp(io);
  tcp::endpoint endpoint(asio::ip::make_address(ip), port);
  conn.async_connect(qp, endpoint, asio::const_buffer{},
                     asio::mutable_buffer{},
                     [&](asio::error_code ec, std::size_t) {
                       completed = true;
                       got = ec;
                       watchdog.cancel();
                       io.stop();
                     });

  io.run();
  bool const ok = completed && static_cast<bool>(got);
  if (ok) {
    std::cout << "[PASS] " << name << ": " << got.message() << "\n";
  } else if (!completed) {
    std::cerr << "[FAIL] " << name << ": watchdog timeout\n";
  } else {
    std::cerr << "[FAIL] " << name << ": unexpectedly connected\n";
    conn.disconnect();
  }
  return ok;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cout << "[SKIP] usage: " << argv[0] << " <roce-ip> [port] "
              << "(needs a working RDMA device + IP)\n";
    return 0;
  }

  std::string ip = argv[1];
  auto const base_port =
      argc > 2 ? static_cast<std::uint16_t>(std::stoi(argv[2]))
               : static_cast<std::uint16_t>(5100);

  try {
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device({});
    if (!device) {
      std::cout << "[SKIP] no RDMA device available\n";
      return 0;
    }

    bool ok = true;
    ok &= phase_open_guards(device);
    ok &= phase_queue_pair_and_cq_guards(device);
    ok &= phase_memory_region_boundaries(device);
    ok &= phase_read_write(device, ip, base_port);
    ok &= phase_zero_length(device, ip, static_cast<std::uint16_t>(base_port + 10));
    ok &= phase_multi_message_order(
        device, ip, static_cast<std::uint16_t>(base_port + 20));
    ok &= phase_negative_connect(
        device, ip, static_cast<std::uint16_t>(base_port + 30));

    if (ok) {
      std::cout << "\nAll rdma regression tests passed.\n";
      return 0;
    }
    std::cerr << "\n[FAIL] one or more rdma regression phases failed.\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
