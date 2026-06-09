// Stage 2: per-op (cancellation_slot) cancellation of control-plane ops.
// Verifies that cancel_after / co_spawn-cancel / awaitable_operators || act on an
// individual control-plane operation, and that the post-cancel object semantics
// hold:
//
//   Phase 1 -- async_connect: a server takes the connect REQUEST but never
//     accepts, so the client is parked in `connecting`. A cancellation_signal
//     (emitted from another thread) cancels just that connect -> operation_aborted,
//     and the connector is left terminal (reconnect -> ext_connector_terminal).
//
//   Phase 2 -- listener async_get_connection: with no client, the get is parked;
//     a cancellation_signal cancels it -> operation_aborted, and the listener is
//     still usable (a subsequent client connect makes a second get_connection
//     succeed). Proves the associator-forwarded slot actually reaches the op.
//
//   Phase 3 -- async_wait_disconnect via `||`: on an established connection,
//     `co_await (conn.async_wait_disconnect || timer)` with the timer winning
//     cancels the watcher per-op WITHOUT tearing the connection down -- a
//     subsequent async_send on the same QP still succeeds.
//
// Usage: test_ibv_control_cancel <roce-ip> [port]  (skips if no arg).
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <variant>

#include "asio/awaitable.hpp"
#include "asio/as_tuple.hpp"
#include "asio/bind_cancellation_slot.hpp"
#include "asio/cancellation_signal.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/awaitable_operators.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
using namespace std::chrono_literals;
using namespace asio::experimental::awaitable_operators;

constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget) {
  auto const deadline = std::chrono::steady_clock::now() + budget;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Phase 1: cancel an in-flight async_connect (per-op slot), connector terminal.
// ---------------------------------------------------------------------------
bool phase_connect(rdma::rdma_device_ptr const& device, std::string const& ip,
                   uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(tcp::endpoint(asio::ip::address_v4::any(), port));
  lis.listen();

  rdma::rdma_connector<tcp> cli(io);
  cli.open(tcp::v4());
  rdma::rdma_queue_pair qp_c(io);

  std::atomic<bool> got_req{false}, conn_done{false};
  asio::error_code conn_ec;

  // Server: take the REQUEST but never accept -> client stays `connecting`.
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, sconn] = co_await lis.async_get_connection(nothrow);
        if (ecg) co_return;
        got_req.store(true, std::memory_order_release);
        asio::steady_timer park(io);
        park.expires_after(60s);
        co_await park.async_wait(nothrow);
      },
      asio::detached);

  asio::cancellation_signal sig;
  tcp::endpoint ep(asio::ip::make_address(ip), port);
  std::string req = "c";
  cli.async_connect(qp_c, ep, asio::buffer(req),
                    asio::bind_cancellation_slot(
                        sig.slot(), [&](asio::error_code ec) {
                          conn_ec = ec;
                          conn_done.store(true, std::memory_order_release);
                        }));

  std::thread worker([&] { io.run(); });

  bool const reqd = wait_until(
      [&] { return got_req.load(std::memory_order_acquire); }, 5s);
  std::this_thread::sleep_for(100ms);
  sig.emit(asio::cancellation_type::terminal);  // cancel just this connect

  bool const fired = wait_until(
      [&] { return conn_done.load(std::memory_order_acquire); }, 5s);

  // Connector is now terminal: a reconnect must be rejected up front.
  std::atomic<bool> re_done{false};
  asio::error_code re_ec;
  cli.async_connect(qp_c, ep, asio::buffer(req), [&](asio::error_code ec) {
    re_ec = ec;
    re_done.store(true, std::memory_order_release);
  });
  bool const re_fired = wait_until(
      [&] { return re_done.load(std::memory_order_acquire); }, 5s);

  io.stop();
  worker.join();

  bool const ok = reqd && fired && conn_ec == asio::error::operation_aborted &&
                  re_fired && re_ec == rdma::ibv_errc::ext_connector_terminal;
  if (ok) {
    std::cout << "[PASS] phase 1: async_connect per-op cancel -> aborted; "
                 "connector terminal (reconnect -> ext_connector_terminal)\n";
  } else {
    std::cerr << "[FAIL] phase 1: reqd=" << reqd << " fired=" << fired
              << " conn_ec=" << conn_ec.message() << " re_ec=" << re_ec.message()
              << "\n";
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Phase 2: cancel a listener async_get_connection; listener stays reusable.
// Single-threaded coroutines (avoids cross-thread op initiation). `||` with a
// timer cancels the get -- since || awaits the cancelled operand, the co_await
// returning at all proves the get was actually cancelled (not hung).
// ---------------------------------------------------------------------------
bool phase_get_connection(rdma::rdma_device_ptr const& device,
                          std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(tcp::endpoint(asio::ip::address_v4::any(), port));
  lis.listen();

  bool g1_cancelled = false, g2_ok = false;

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        // No client connecting: the get is parked; a 300ms timer wins the || and
        // cancels it per-op.
        asio::steady_timer t(io);
        t.expires_after(300ms);
        auto r = co_await (lis.async_get_connection(nothrow) ||
                           t.async_wait(nothrow));
        g1_cancelled = (r.index() == 1);  // timer won -> get was cancelled

        // Reuse: a client connects (sends a REQUEST); a fresh get_connection on
        // the same listener must succeed -> listener stayed in LISTEN.
        asio::co_spawn(
            io,
            [&]() -> asio::awaitable<void> {
              rdma::rdma_connector<tcp> cli(io);
              cli.open(tcp::v4());
              rdma::rdma_queue_pair qp(io);
              tcp::endpoint ep(asio::ip::make_address(ip), port);
              std::string req = "c";
              co_await cli.async_connect(qp, ep, asio::buffer(req), nothrow);
            },
            asio::detached);

        auto [e2, c2] = co_await lis.async_get_connection(nothrow);
        g2_ok = (!e2);
        io.stop();
      },
      asio::detached);

  io.run();

  bool const ok = g1_cancelled && g2_ok;
  if (ok) {
    std::cout << "[PASS] phase 2: get_connection per-op cancel (||); listener "
                 "still usable (next get_connection succeeded)\n";
  } else {
    std::cerr << "[FAIL] phase 2: g1_cancelled=" << g1_cancelled
              << " g2_ok=" << g2_ok << "\n";
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Phase 3: cancel async_wait_disconnect via `||`; connection stays alive.
// ---------------------------------------------------------------------------
bool phase_wait_disconnect(rdma::rdma_device_ptr const& device,
                           std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(tcp::endpoint(asio::ip::address_v4::any(), port));
  lis.listen();

  bool established = false, wait_cancelled = false, alive_after = false;

  // Server: accept, receive the client's "alive" probe, then wait for disconnect.
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, conn] = co_await lis.async_get_connection(nothrow);
        if (ecg) { io.stop(); co_return; }
        rdma::rdma_queue_pair qp_s(io);
        std::string rep = "s";
        auto [eca] = co_await conn.async_accept(qp_s, asio::buffer(rep), nothrow);
        if (eca) { io.stop(); co_return; }
        std::array<char, 64> buf{};
        rdma::rdma_memory_region mr(device, buf.data(), buf.size());
        co_await qp_s.async_recv(mr.slice(std::size_t{0}, buf.size()), nothrow);
        co_await conn.async_wait_disconnect(nothrow);  // until client disconnects
        io.stop();
      },
      asio::detached);

  // Client: connect, race wait_disconnect against a timer (timer wins -> watcher
  // cancelled per-op), then prove the connection is still alive with a send.
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(tcp::v4());
        rdma::rdma_queue_pair qp_c(io);
        tcp::endpoint ep(asio::ip::make_address(ip), port);
        std::string req = "c";
        auto [ecc] = co_await conn.async_connect(qp_c, ep, asio::buffer(req), nothrow);
        if (ecc) co_return;
        established = true;

        asio::steady_timer t(io);
        t.expires_after(300ms);
        auto r = co_await (conn.async_wait_disconnect(nothrow) ||
                           t.async_wait(nothrow));
        wait_cancelled = (r.index() == 1);  // timer won -> watcher was cancelled

        std::array<char, 64> sbuf{};
        std::memcpy(sbuf.data(), "alive", 5);
        rdma::rdma_memory_region smr(device, sbuf.data(), sbuf.size());
        auto [sec, sn] = co_await qp_c.async_send(smr.cslice(std::size_t{0}, 5),
                                                  nothrow);
        (void)sn;
        alive_after = !sec;
        conn.disconnect();
      },
      asio::detached);

  io.run();

  bool const ok = established && wait_cancelled && alive_after;
  if (ok) {
    std::cout << "[PASS] phase 3: async_wait_disconnect cancelled by || (timer); "
                 "connection still alive (async_send succeeded after)\n";
  } else {
    std::cerr << "[FAIL] phase 3: established=" << established
              << " wait_cancelled=" << wait_cancelled
              << " alive_after=" << alive_after << "\n";
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
  uint16_t port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 5040;

  try {
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    bool ok = true;
    ok &= phase_connect(device, ip, port);
    ok &= phase_get_connection(device, ip, static_cast<uint16_t>(port + 1));
    ok &= phase_wait_disconnect(device, ip, static_cast<uint16_t>(port + 2));

    if (ok) {
      std::cout << "\nAll ibv control-plane cancellation tests passed.\n";
      return 0;
    }
    std::cerr << "\n[FAIL] one or more phases failed.\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
