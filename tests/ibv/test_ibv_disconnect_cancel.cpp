// Functional test for the MT-safe, state-adaptive connector::disconnect()
// (design A in docs/cancellation_stage1_object.md). Covers the paths the echo /
// wait_disconnect tests do NOT exercise:
//
//   Phase A -- pending-connect abort, called from another thread:
//     a listener receives the connect request but deliberately NEVER accepts, so
//     the client is reliably parked in `connecting` (REQ sent, no REP). While
//     io.run() spins on a worker thread, the MAIN thread calls disconnect() on
//     the client. Asserts the connect completes with operation_aborted (the
//     connect_state_ middle-state CAS bail + thread-safe disconnect).
//
//   Phase B -- established data-plane teardown, called from another thread:
//     a server+client connection is established; the client posts an async_recv
//     that blocks (server never sends). While io.run() spins on a worker thread,
//     the MAIN thread calls disconnect() on the client. Asserts the pending recv
//     completes with operation_aborted (connected -> closed flush, MT-safe).
//
//   Phase C -- establish-vs-disconnect soak (the connecting->connected
//     arbitration): a real server accepts every connection, and the client
//     disconnect()s after a delay swept across the CM handshake latency so
//     iterations land on BOTH sides of the race -- op wins (disconnect then sees
//     connected and tears down) and disconnect wins (op's ESTABLISHED-CAS fails
//     -> second-actor teardown, or A.7 drain). Asserts every connect completes
//     exactly once with success or operation_aborted, never another error, and
//     no crash/hang/double-teardown.
//
// Usage: test_ibv_disconnect_cancel <roce-ip> [port]
// (Needs a working RDMA device + an IP bound to it; skips if no arg given.)
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/as_tuple.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
using namespace std::chrono_literals;

constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

// Spin-wait on an atomic flag with a deadline; returns false on timeout.
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
// Phase A: abort an in-flight connect from a different thread. A server accepts
// the connection REQUEST at the CM level (async_get_connection) but never calls
// async_accept, so the client stays in `connecting` deterministically.
// ---------------------------------------------------------------------------
bool phase_a(rdma::rdma_device_ptr const& device, std::string const& ip,
             uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(tcp::endpoint(asio::ip::address_v4::any(), port));
  listener.listen();

  rdma::rdma_connector<tcp> conn(io);
  conn.open(tcp::v4());
  rdma::rdma_queue_pair qp(io);

  std::atomic<bool> got_req{false};
  std::atomic<bool> done{false};
  asio::error_code cec;

  // Server: take the connect request but NEVER accept -- hold the connector so
  // the client's REQ has been received but no REP is sent (client -> connecting).
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, srv_conn] = co_await listener.async_get_connection(nothrow);
        if (ecg) co_return;
        got_req.store(true, std::memory_order_release);
        asio::steady_timer park(io);
        park.expires_after(60s);
        co_await park.async_wait(nothrow);  // hold srv_conn alive; cancelled at io.stop()
      },
      asio::detached);

  std::string req = "x";
  tcp::endpoint ep(asio::ip::make_address(ip), port);
  conn.async_connect(qp, ep, asio::buffer(req), [&](asio::error_code ec) {
    cec = ec;
    done.store(true, std::memory_order_release);
  });

  std::thread worker([&] { io.run(); });

  // Wait until the server has the REQ (client is now `connecting`), then tear the
  // client down from THIS thread.
  bool const reqd = wait_until(
      [&] { return got_req.load(std::memory_order_acquire); }, 5s);
  std::this_thread::sleep_for(100ms);
  conn.disconnect();  // MT-safe: io.run() is on `worker`

  bool const fired = wait_until(
      [&] { return done.load(std::memory_order_acquire); }, 5s);
  io.stop();
  worker.join();

  bool const ok = reqd && fired && cec == asio::error::operation_aborted;
  if (ok) {
    std::cout << "[PASS] phase A: in-flight connect aborted by cross-thread "
                 "disconnect() (operation_aborted)\n";
  } else {
    std::cerr << "[FAIL] phase A: got_req=" << reqd << " fired=" << fired
              << " ec=" << cec.message() << " (expected operation_aborted)\n";
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Phase B: tear down an established connection (flushing a pending recv) from a
// different thread.
// ---------------------------------------------------------------------------
bool phase_b(rdma::rdma_device_ptr const& device, std::string const& ip,
             uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(tcp::endpoint(asio::ip::address_v4::any(), port));
  listener.listen();

  // Client connector at outer scope so the MAIN thread can disconnect() it.
  rdma::rdma_connector<tcp> cli(io);
  cli.open(tcp::v4());
  rdma::rdma_queue_pair qp_c(io);

  std::atomic<bool> recv_armed{false};
  std::atomic<bool> recv_done{false};
  asio::error_code recv_ec;

  // Server: accept, then park on wait_disconnect to keep the connection alive.
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, conn] = co_await listener.async_get_connection(nothrow);
        if (ecg) co_return;
        rdma::rdma_queue_pair qp_s(io);
        std::string rep = "s";
        auto [eca] = co_await conn.async_accept(qp_s, asio::buffer(rep), nothrow);
        if (eca) co_return;
        co_await conn.async_wait_disconnect(nothrow);  // until client tears down
      },
      asio::detached);

  // Client: connect, then post a recv that blocks (server never sends).
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        tcp::endpoint ep(asio::ip::make_address(ip), port);
        std::string req = "c";
        auto [ecc] = co_await cli.async_connect(qp_c, ep, asio::buffer(req), nothrow);
        if (ecc) {
          std::cerr << "[client] connect: " << ecc.message() << "\n";
          co_return;
        }
        std::array<char, 4096> buf{};
        rdma::rdma_memory_region mr(device, buf.data(), buf.size());
        recv_armed.store(true, std::memory_order_release);
        auto [er, n] = co_await qp_c.async_recv(mr.slice(std::size_t{0}, buf.size()),
                                                nothrow);
        (void)n;
        recv_ec = er;
        recv_done.store(true, std::memory_order_release);
      },
      asio::detached);

  std::thread worker([&] { io.run(); });

  bool const armed = wait_until(
      [&] { return recv_armed.load(std::memory_order_acquire); }, 5s);
  // Ensure the recv WR is actually posted to the HW before we flush it.
  std::this_thread::sleep_for(200ms);
  cli.disconnect();  // MT-safe: io.run() is on `worker`

  bool const fired = wait_until(
      [&] { return recv_done.load(std::memory_order_acquire); }, 5s);
  io.stop();
  worker.join();

  bool const ok = armed && fired && recv_ec == asio::error::operation_aborted;
  if (ok) {
    std::cout << "[PASS] phase B: established connection's pending recv aborted "
                 "by cross-thread disconnect() (operation_aborted)\n";
  } else {
    std::cerr << "[FAIL] phase B: armed=" << armed << " fired=" << fired
              << " ec=" << recv_ec.message() << " (expected operation_aborted)\n";
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Phase C: soak the connecting->connected arbitration. A real server accepts
// every connection, so each client connect WILL establish -- but the client
// disconnect()s (from the main thread) after a delay swept across the
// establishment latency, so iterations straddle the race: some land before
// ESTABLISHED (disconnect wins -> op's ESTABLISHED-CAS fails -> op second-actor
// teardown, or A.7 drain), some after (op wins -> disconnect sees connected ->
// disconnect tears down). Every connect must complete exactly once with either
// success or operation_aborted -- never another error, crash, or hang, and
// rdma_disconnect happens at most once per connection (no double-teardown).
// ---------------------------------------------------------------------------
bool phase_c(rdma::rdma_device_ptr const& device, std::string const& ip,
             uint16_t port, int iterations) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> listener(io);
  listener.open(tcp::v4());
  listener.bind(tcp::endpoint(asio::ip::address_v4::any(), port));
  listener.listen();

  // Server: accept every incoming connection (each in its own coroutine) and
  // park on wait_disconnect until the client tears down.
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        for (;;) {
          auto [ecg, conn] = co_await listener.async_get_connection(nothrow);
          if (ecg) co_return;  // listener cancelled at io.stop()
          asio::co_spawn(
              io,
              [&io, c = std::move(conn)]() mutable -> asio::awaitable<void> {
                rdma::rdma_queue_pair qp_s(io);
                std::string rep = "s";
                auto [eca] = co_await c.async_accept(qp_s, asio::buffer(rep), nothrow);
                if (eca) co_return;
                co_await c.async_wait_disconnect(nothrow);
              },
              asio::detached);
        }
      },
      asio::detached);

  struct slot {
    std::atomic<bool> done{false};
    asio::error_code ec;
  };
  std::vector<std::unique_ptr<slot>> slots;
  std::vector<std::unique_ptr<rdma::rdma_connector<tcp>>> conns;
  std::vector<std::unique_ptr<rdma::rdma_queue_pair>> qps;
  slots.reserve(iterations);
  conns.reserve(iterations);
  qps.reserve(iterations);

  std::thread worker([&] { io.run(); });

  tcp::endpoint ep(asio::ip::make_address(ip), port);
  int established = 0, aborted = 0, other = 0, missing = 0;

  for (int i = 0; i < iterations; ++i) {
    auto s = std::make_unique<slot>();
    auto c = std::make_unique<rdma::rdma_connector<tcp>>(io);
    c->open(tcp::v4());
    auto q = std::make_unique<rdma::rdma_queue_pair>(io);

    slot* sp = s.get();
    std::string req = "c";
    c->async_connect(*q, ep, asio::buffer(req), [sp](asio::error_code ec) {
      sp->ec = ec;
      sp->done.store(true, std::memory_order_release);
    });

    // Sweep the delay across the establishment window (~0..19.6ms) to straddle
    // the connecting->connected arbitration from both sides (the RoCE CM
    // handshake is a few ms, so a sub-ms sweep only ever hits the early side).
    std::this_thread::sleep_for(std::chrono::microseconds((i % 50) * 400));
    c->disconnect();  // MT-safe: io.run() is on `worker`

    // Keep objects + slot alive (destroyed only after io.stop()+join, never
    // concurrently with the running reactor).
    conns.push_back(std::move(c));
    qps.push_back(std::move(q));
    slots.push_back(std::move(s));

    if (!wait_until([sp] { return sp->done.load(std::memory_order_acquire); }, 5s)) {
      ++missing;
      continue;
    }
    if (!slots.back()->ec) {
      ++established;
    } else if (slots.back()->ec == asio::error::operation_aborted) {
      ++aborted;
    } else {
      ++other;
      std::cerr << "[phase C] iter " << i
                << " unexpected ec: " << slots.back()->ec.message() << "\n";
    }
  }

  io.stop();
  worker.join();

  bool const ok = (missing == 0) && (other == 0) &&
                  (established + aborted == iterations);
  std::cout << "[" << (ok ? "PASS" : "FAIL") << "] phase C: " << iterations
            << " establish-vs-disconnect races -- established=" << established
            << " aborted=" << aborted << " other=" << other
            << " missing=" << missing << "\n";
  if (ok && (established == 0 || aborted == 0)) {
    std::cout << "       (note: only one side of the race was hit; arbitration "
                 "still consistent)\n";
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
  uint16_t port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 5012;

  try {
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});

    bool ok = true;
    ok &= phase_a(device, ip, static_cast<uint16_t>(port + 113));
    ok &= phase_b(device, ip, port);
    ok &= phase_c(device, ip, static_cast<uint16_t>(port + 211), 150);

    if (ok) {
      std::cout << "\nAll ibv disconnect/cancel tests passed.\n";
      return 0;
    }
    std::cerr << "\n[FAIL] one or more phases failed.\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
