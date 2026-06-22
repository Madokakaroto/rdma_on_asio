// Cross-platform RDMA dual-family echo test.
//
// On ONE device and a SINGLE use_device(), run a v4 echo round, then -- after it
// completes -- a v6 echo round. This proves a single device serves both address
// families: on nd a NIC's v4/v6 addresses are grouped into one device (one
// OpenAdapter / one PD) by AdapterId; on ibv the device is family-agnostic and
// the family is consumed at rdma_cm connect time. See docs/nd_dual_family_plan.md.
//
// Each round uses a FRESH connector/listener/queue_pair: connectors are one-shot,
// and a listener binds a single family chosen at open(ps). The memory region is
// registered once per round on the same dual-family device (MRs are family-agnostic).
//
// Two-process form (mirrors test_rdma_echo.cpp):
//   --server [--port P]
//   --client-v4 HOST4 [--client-v6 HOST6] [--port P]
// The server runs the v4 round then the v6 round on the same port (sequential,
// so the v4 listener is gone before the v6 one binds). The client runs whichever
// rounds it was given a host for.
#include <array>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);
constexpr std::size_t kBufSize = 4096;
constexpr int kEchoCount = 5;

// One server-side echo round on the given port space (v4 or v6). Returns true on
// a clean round, false on setup/transfer failure (e.g. the device has no address
// of this family -> rdma_errc::address_family_not_supported at bind).
asio::awaitable<bool> server_round(asio::io_context& io_ctx,
                                   rdma::rdma_device_ptr const& device,
                                   tcp ps, std::string_view tag, uint16_t port) {
  rdma::rdma_listener<tcp> listener(io_ctx);
  asio::error_code ec;
  listener.open(ps, ec);
  if (ec) {
    std::cerr << "[server " << tag << "] open: " << ec.message() << "\n";
    co_return false;
  }
  listener.bind(port, ec);
  if (ec) {
    std::cerr << "[server " << tag << "] bind: " << ec.message() << "\n";
    co_return false;
  }
  listener.listen(128, ec);
  if (ec) {
    std::cerr << "[server " << tag << "] listen: " << ec.message() << "\n";
    co_return false;
  }
  std::cout << "[server " << tag << "] listening on port " << port << "\n";

  std::array<char, 256> req_pd_buf{};
  auto [ec_get, conn, req_pd_len] = co_await listener.async_get_connection(
      asio::buffer(req_pd_buf), use_nothrow);
  if (ec_get) {
    std::cerr << "[server " << tag << "] get_connection: " << ec_get.message()
              << "\n";
    co_return false;
  }

  rdma::rdma_queue_pair qp(io_ctx);
  std::string reply_pd = "server-hello";
  auto [ec_accept] =
      co_await conn.async_accept(qp, asio::buffer(reply_pd), use_nothrow);
  if (ec_accept) {
    std::cerr << "[server " << tag << "] accept: " << ec_accept.message() << "\n";
    co_return false;
  }
  std::cout << "[server " << tag << "] connection accepted\n";

  std::array<char, kBufSize> raw_buf{};
  rdma::rdma_memory_region mr(device, raw_buf.data(), raw_buf.size());

  bool ok = true;
  for (int i = 0; i < kEchoCount; ++i) {
    auto recv_buf = mr.slice(std::size_t{0}, kBufSize);
    auto [ec_recv, n] = co_await qp.async_recv(recv_buf, use_nothrow);
    if (ec_recv || n == 0) {
      if (ec_recv && ec_recv != asio::error::operation_aborted) {
        std::cerr << "[server " << tag << "] recv: " << ec_recv.message() << "\n";
        ok = false;
      }
      break;
    }
    auto send_buf = mr.cslice(std::size_t{0}, n);
    auto [ec_send, sent] = co_await qp.async_send(send_buf, use_nothrow);
    if (ec_send) {
      std::cerr << "[server " << tag << "] send: " << ec_send.message() << "\n";
      ok = false;
      break;
    }
  }

  asio::error_code ec_disc;
  conn.disconnect(ec_disc);
  std::cout << "[server " << tag << "] round done\n";
  co_return ok;
}

// One client-side echo round to host:port over the given port space.
asio::awaitable<bool> client_round(asio::io_context& io_ctx,
                                   rdma::rdma_device_ptr const& device,
                                   tcp ps, std::string_view tag,
                                   std::string const& host, uint16_t port) {
  rdma::rdma_connector<tcp> conn(io_ctx);
  asio::error_code ec;
  conn.open(ps, ec);  // optional; async_connect auto-opens otherwise
  if (ec) {
    std::cerr << "[client " << tag << "] open: " << ec.message() << "\n";
    co_return false;
  }
  rdma::rdma_queue_pair qp(io_ctx);

  tcp::endpoint endpoint(asio::ip::make_address(host), port);
  std::string req_pd = "client-hello";
  std::array<char, 256> reply_pd_buf{};
  auto [ec_conn, reply_pd_len] = co_await conn.async_connect(
      qp, endpoint, asio::buffer(req_pd), asio::buffer(reply_pd_buf),
      use_nothrow);
  if (ec_conn) {
    std::cerr << "[client " << tag << "] connect: " << ec_conn.message() << "\n";
    co_return false;
  }
  std::cout << "[client " << tag << "] connected to " << host << ":" << port
            << "\n";

  std::array<char, kBufSize> raw_buf{};
  rdma::rdma_memory_region mr(device, raw_buf.data(), raw_buf.size());

  bool ok = true;
  for (int i = 0; i < kEchoCount; ++i) {
    std::string msg = std::string(tag) + " Hello RDMA #" + std::to_string(i);
    std::memcpy(raw_buf.data(), msg.data(), msg.size());

    auto send_buf = mr.cslice(std::size_t{0}, msg.size());
    auto [ec_send, sent] = co_await qp.async_send(send_buf, use_nothrow);
    if (ec_send) {
      std::cerr << "[client " << tag << "] send: " << ec_send.message() << "\n";
      ok = false;
      break;
    }
    auto recv_buf = mr.slice(std::size_t{0}, kBufSize);
    auto [ec_recv, n] = co_await qp.async_recv(recv_buf, use_nothrow);
    if (ec_recv) {
      std::cerr << "[client " << tag << "] recv: " << ec_recv.message() << "\n";
      ok = false;
      break;
    }
    if (std::string_view(raw_buf.data(), n) != msg) {
      std::cerr << "[client " << tag << "] echo mismatch\n";
      ok = false;
      break;
    }
  }

  asio::error_code ec_disc;
  conn.disconnect(ec_disc);
  std::cout << "[client " << tag << "] round done\n";
  co_return ok;
}

// v4 round first, then v6 round -- same io_context, same device, one use_device.
asio::awaitable<void> run_server(asio::io_context& io_ctx,
                                 rdma::rdma_device_ptr const& device,
                                 uint16_t port) {
  bool const v4 = co_await server_round(io_ctx, device, tcp::v4(), "v4", port);
  std::cout << "[server] v4 round: " << (v4 ? "OK" : "FAILED") << "\n";
  bool const v6 = co_await server_round(io_ctx, device, tcp::v6(), "v6", port);
  std::cout << "[server] v6 round: " << (v6 ? "OK" : "FAILED") << "\n";
}

asio::awaitable<void> run_client(asio::io_context& io_ctx,
                                 rdma::rdma_device_ptr const& device,
                                 std::string const& host_v4,
                                 std::string const& host_v6, uint16_t port) {
  if (!host_v4.empty()) {
    bool const v4 =
        co_await client_round(io_ctx, device, tcp::v4(), "v4", host_v4, port);
    std::cout << "[client] v4 round: " << (v4 ? "OK" : "FAILED") << "\n";
  }
  if (!host_v6.empty()) {
    bool const v6 =
        co_await client_round(io_ctx, device, tcp::v6(), "v6", host_v6, port);
    std::cout << "[client] v6 round: " << (v6 ? "OK" : "FAILED") << "\n";
  }
}

void print_usage(char const* argv0) {
  std::cerr << "Usage:\n"
            << "  " << argv0 << " --server [--port PORT]\n"
            << "  " << argv0
            << " --client-v4 HOST4 [--client-v6 HOST6] [--port PORT]\n";
}

int main(int argc, char* argv[]) {
  bool is_server = false;
  std::string host_v4;
  std::string host_v6;
  uint16_t port = 5000;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--server") {
      is_server = true;
    } else if (arg == "--client-v4" && i + 1 < argc) {
      host_v4 = argv[++i];
    } else if (arg == "--client-v6" && i + 1 < argc) {
      host_v6 = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::stoi(argv[++i]));
    }
  }

  bool const is_client = !host_v4.empty() || !host_v6.empty();
  if (!is_server && !is_client) {
    print_usage(argv[0]);
    return 1;
  }

  try {
    asio::io_context io_ctx;
    auto device =
        rdma::rdma_device_manager_t::instance().get_first_available_device({});
    rdma::use_device(io_ctx, device);  // ONE registration serves both families

    auto on_done = [&io_ctx](std::exception_ptr) { io_ctx.stop(); };
    if (is_server) {
      asio::co_spawn(io_ctx, run_server(io_ctx, device, port), on_done);
    } else {
      asio::co_spawn(io_ctx, run_client(io_ctx, device, host_v4, host_v6, port),
                     on_done);
    }

    io_ctx.run();
    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
