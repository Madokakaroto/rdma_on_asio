// Connect/accept private-data test (cross-platform; rdma_* API only).
//
// Validates the symmetric send/recv private-data design (docs/connect_private_data_plan.md):
//   - SEND via const_buffer arg (connect request / accept reply); RECEIVE via
//     mutable out-param (get_connection request / connect reply) + a size_t
//     completion giving bytes-written.
//
//   Phase 1 -- asymmetric matrix {client_req in {empty, "REQ.."}}
//                              x {server_reply in {empty, "REP.."}} (4 combos):
//     the connection establishes regardless; the receiver's buffer prefix equals
//     exactly the bytes the sender sent (trailing bytes are rdma_cm zero/padding,
//     so we only check the sent-length prefix). One direction sending while the
//     other does not is fully legal.
//
//   Phase 2 -- outgoing-request lifetime: the client passes a request buffer that
//     is DESTROYED before io.run() drives the handshake (a scoped temporary +
//     callback). Because the library copies the request into the op at initiation
//     (connect issues rdma_connect asynchronously, after resolve), the server
//     still receives the intended bytes -- proving lifetime is library-managed,
//     not caller-managed.
//
// Usage: test_rdma_private_data <roce-ip> [port]   (skips if no arg).
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/buffer.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/experimental/awaitable_operators.hpp"
#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;
using namespace asio::experimental::awaitable_operators;
using namespace std::chrono_literals;

constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

// The receiver's buffer is filled with the sender's bytes followed by transport
// padding, so only the sent-length prefix is meaningful.
bool prefix_ok(std::string_view got, std::string_view sent) {
  return sent.empty() ||
         (got.size() >= sent.size() && got.substr(0, sent.size()) == sent);
}

struct outcome {
  bool established = false;
  std::string got_request;  // what the server received (req_len bytes)
  std::string got_reply;    // what the client received (reply_len bytes)
};

// One full connection with the given request/reply private data; captures what
// each side received via the symmetric out-params.
outcome run_once(rdma::rdma_device_ptr const& device, std::string const& ip,
                 uint16_t port, std::string client_request,
                 std::string server_reply) {
  asio::io_context io;
  rdma::use_device(io, device);
  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(port);
  lis.listen();

  outcome out;
  std::atomic<int> remaining{2};
  auto on_done = [&](std::exception_ptr) {
    if (--remaining == 0) io.stop();
  };

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        std::array<char, 256> req_buf{};
        auto [ecg, conn, req_len] =
            co_await lis.async_get_connection(asio::buffer(req_buf), nothrow);
        if (ecg) co_return;
        out.got_request.assign(req_buf.data(), req_len);
        rdma::rdma_queue_pair qp(io);
        auto [eca] =
            co_await conn.async_accept(qp, asio::buffer(server_reply), nothrow);
        if (eca) co_return;
        co_await conn.async_wait_disconnect(nothrow);
      },
      on_done);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(tcp::v4());
        rdma::rdma_queue_pair qp(io);
        tcp::endpoint ep(asio::ip::make_address(ip), port);
        std::array<char, 256> reply_buf{};
        auto [ecc, reply_len] = co_await conn.async_connect(
            qp, ep, asio::buffer(client_request), asio::buffer(reply_buf),
            nothrow);
        if (ecc) co_return;
        out.established = true;
        out.got_reply.assign(reply_buf.data(), reply_len);
        conn.disconnect();
      },
      on_done);

  io.run();
  return out;
}

bool phase_matrix(rdma::rdma_device_ptr const& device, std::string const& ip,
                  uint16_t base_port) {
  struct combo {
    std::string req, reply;
  };
  combo const combos[] = {
      {"", ""},
      {"REQ-from-client", ""},
      {"", "REP-from-server"},
      {"REQ-from-client", "REP-from-server"},
  };
  bool ok = true;
  uint16_t port = base_port;
  for (auto const& c : combos) {
    auto r = run_once(device, ip, port++, c.req, c.reply);
    bool const this_ok = r.established && prefix_ok(r.got_request, c.req) &&
                         prefix_ok(r.got_reply, c.reply);
    ok &= this_ok;
    if (!this_ok) {
      std::cerr << "[FAIL] matrix combo req=\"" << c.req << "\" reply=\""
                << c.reply << "\": established=" << r.established
                << " got_request(prefix)=\""
                << std::string_view(r.got_request).substr(0, c.req.size())
                << "\" got_reply(prefix)=\""
                << std::string_view(r.got_reply).substr(0, c.reply.size())
                << "\"\n";
    }
  }
  if (ok) {
    std::cout << "[PASS] phase 1: asymmetric request/reply matrix (4 combos) -- "
                 "each side receives exactly the bytes the peer sent\n";
  }
  return ok;
}

bool phase_request_lifetime(rdma::rdma_device_ptr const& device,
                            std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);
  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(port);
  lis.listen();

  std::string got_request;
  std::atomic<bool> conn_done{false};

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        std::array<char, 256> req_buf{};
        auto [ecg, conn, req_len] =
            co_await lis.async_get_connection(asio::buffer(req_buf), nothrow);
        if (ecg) { io.stop(); co_return; }
        got_request.assign(req_buf.data(), req_len);
        rdma::rdma_queue_pair qp(io);
        co_await conn.async_accept(qp, asio::const_buffer{}, nothrow);
        co_await conn.async_wait_disconnect(nothrow);
        io.stop();
      },
      asio::detached);

  rdma::rdma_connector<tcp> conn(io);
  conn.open(tcp::v4());
  rdma::rdma_queue_pair qp(io);
  tcp::endpoint ep(asio::ip::make_address(ip), port);
  std::array<char, 256> reply_buf{};

  // The request buffer is a TEMPORARY destroyed at the end of this block --
  // before io.run() drives the CM handshake / issues rdma_connect. The library
  // copies the request into the op at initiation, so the server must still see it.
  {
    std::string ephemeral_request = "temp-request-xyz";
    conn.async_connect(qp, ep, asio::buffer(ephemeral_request),
                       asio::buffer(reply_buf),
                       [&](asio::error_code ec, std::size_t) {
                         conn_done.store(true, std::memory_order_release);
                         if (!ec) conn.disconnect();  // let the server unblock
                       });
  }  // ephemeral_request destroyed HERE, before io.run()

  io.run();

  bool const ok = conn_done.load(std::memory_order_acquire) &&
                  prefix_ok(got_request, "temp-request-xyz");
  if (ok) {
    std::cout << "[PASS] phase 2: outgoing request copied at initiation -- a "
                 "destroyed-before-handshake temporary still arrives intact\n";
  } else {
    std::cerr << "[FAIL] phase 2: got_request(prefix)=\""
              << std::string_view(got_request).substr(0, 16) << "\"\n";
  }
  return ok;
}

// The "without response pd" convenience overloads: client connects without a
// reply buffer (completion void(ec), no reply_len), server accepts without a
// reply. The request still flows; the connection still establishes.
bool phase_no_reply(rdma::rdma_device_ptr const& device, std::string const& ip,
                    uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);
  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(port);
  lis.listen();

  std::string got_request;
  bool established = false;
  std::atomic<int> remaining{2};
  auto on_done = [&](std::exception_ptr) {
    if (--remaining == 0) io.stop();
  };

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        std::array<char, 256> req_buf{};
        auto [ecg, conn, req_len] =
            co_await lis.async_get_connection(asio::buffer(req_buf), nothrow);
        if (ecg) co_return;
        got_request.assign(req_buf.data(), req_len);
        rdma::rdma_queue_pair qp(io);
        auto [eca] = co_await conn.async_accept(qp, nothrow);  // no-reply accept
        if (eca) co_return;
        co_await conn.async_wait_disconnect(nothrow);
      },
      on_done);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(tcp::v4());
        rdma::rdma_queue_pair qp(io);
        tcp::endpoint ep(asio::ip::make_address(ip), port);
        std::string req = "REQ-no-reply";
        // No-reply connect overload: completion is void(ec), no reply_len.
        auto [ecc] = co_await conn.async_connect(qp, ep, asio::buffer(req),
                                                 nothrow);
        if (ecc) co_return;
        established = true;
        conn.disconnect();
      },
      on_done);

  io.run();

  bool const ok = established && prefix_ok(got_request, "REQ-no-reply");
  if (ok) {
    std::cout << "[PASS] phase 3: no-reply convenience overloads "
                 "(async_connect / async_accept without response pd)\n";
  } else {
    std::cerr << "[FAIL] phase 3: established=" << established
              << " got_request(prefix)=\""
              << std::string_view(got_request).substr(0, 12) << "\"\n";
  }
  return ok;
}

// Per-op cancellation MUST still work through the no-reply async_connect
// overload -- i.e. the connect_drop_reply_adapter's associator specialization
// really forwards the cancellation_slot to the underlying op. A server takes the
// REQUEST but never accepts, parking the client in `connecting`; the no-reply
// connect is raced against a timer via `||`. The timer wins and cancels the
// connect; since `||` awaits the cancelled operand, the co_await returning at all
// proves the cancel reached the op (broken forwarding -> the connect ignores the
// cancel -> `||` hangs -> the harness times out).
bool phase_no_reply_cancel(rdma::rdma_device_ptr const& device,
                           std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);
  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(port);
  lis.listen();

  bool cancelled = false;

  // Server: take the connect REQUEST but NEVER accept -> client stays connecting.
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, conn, rqn] =
            co_await lis.async_get_connection(asio::mutable_buffer{}, nothrow);
        if (ecg) co_return;
        asio::steady_timer park(io);
        park.expires_after(60s);
        co_await park.async_wait(nothrow);  // hold conn; never accept
      },
      asio::detached);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(tcp::v4());
        rdma::rdma_queue_pair qp(io);
        tcp::endpoint ep(asio::ip::make_address(ip), port);
        std::string req = "c";
        asio::steady_timer t(io);
        t.expires_after(500ms);
        // No-reply connect overload (completion void(ec)); cancelled per-op when
        // the timer wins the ||.
        auto r = co_await (conn.async_connect(qp, ep, asio::buffer(req), nothrow) ||
                           t.async_wait(nothrow));
        cancelled = (r.index() == 1);  // timer won -> the connect was cancelled
        io.stop();
      },
      asio::detached);

  io.run();

  if (cancelled) {
    std::cout << "[PASS] phase 4: per-op cancel reaches the no-reply async_connect "
                 "(associator forwards the cancellation_slot through the adapter)\n";
  } else {
    std::cerr << "[FAIL] phase 4: no-reply async_connect was not cancelled\n";
  }
  return cancelled;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cout << "[SKIP] usage: " << argv[0] << " <roce-ip> [port] "
              << "(needs a working RDMA device + IP)\n";
    return 0;
  }
  std::string ip = argv[1];
  uint16_t port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 5080;

  try {
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device({});
    bool ok = true;
    ok &= phase_matrix(device, ip, port);
    ok &= phase_request_lifetime(device, ip, static_cast<uint16_t>(port + 10));
    ok &= phase_no_reply(device, ip, static_cast<uint16_t>(port + 20));
    ok &= phase_no_reply_cancel(device, ip, static_cast<uint16_t>(port + 30));

    if (ok) {
      std::cout << "\nAll rdma private-data tests passed.\n";
      return 0;
    }
    std::cerr << "\n[FAIL] one or more phases failed.\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
