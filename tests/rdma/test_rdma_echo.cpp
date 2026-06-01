// Cross-platform RDMA echo test. Written entirely against the backend-agnostic
// public surface (rdma/rdma.hpp + rdma_* aliases + tcp port space), so the same
// source compiles and runs on either backend (NetworkDirect / libibverbs).
#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <cstring>

#include "asio/io_context.hpp"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/as_tuple.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);
constexpr std::size_t kBufSize = 4096;
constexpr int kEchoCount = 10;

asio::awaitable<void> run_server(asio::io_context& io_ctx,
                                 rdma::rdma_device_ptr const& device,
                                 uint16_t port) {
  std::cout << "[server] listening on port " << port << "\n";

  rdma::rdma_queue_pair qp(io_ctx);
  rdma::rdma_listener<tcp> listener(io_ctx);
  listener.open();
  listener.bind(tcp::endpoint(asio::ip::address_v4::any(), port));
  listener.listen();

  auto [ec_req, connector, private_data] =
      co_await listener.async_get_connection_request(use_nothrow);
  if (ec_req) {
    std::cerr << "[server] get_connection_request failed: "
              << ec_req.message() << "\n";
    co_return;
  }
  std::cout << "[server] connection request received\n";

  rdma::rdma_connector<tcp> conn(io_ctx);
  conn.open(std::move(connector), qp);

  auto [ec_accept] = co_await conn.async_accept({}, use_nothrow);
  if (ec_accept) {
    std::cerr << "[server] accept failed: " << ec_accept.message() << "\n";
    co_return;
  }
  std::cout << "[server] connection accepted\n";

  std::array<char, kBufSize> raw_buf{};
  rdma::rdma_memory_region mr(device, raw_buf.data(), raw_buf.size());

  // Fixed-count ping-pong: each iteration recv one message and echo it back.
  // (RC QPs don't flush a pending recv on peer disconnect, so we don't rely on
  // a recv error to terminate.)
  for (int i = 0; i < kEchoCount; ++i) {
    auto recv_buf = mr.slice(std::size_t{0}, kBufSize);
    auto [ec_recv, n] = co_await qp.async_recv(recv_buf, use_nothrow);
    if (ec_recv || n == 0) {
      if (ec_recv && ec_recv != asio::error::operation_aborted) {
        std::cerr << "[server] recv error: " << ec_recv.message() << "\n";
      }
      break;
    }
    std::cout << "[server] echo " << n << " bytes: "
              << std::string_view(raw_buf.data(), n) << "\n";

    auto send_buf = mr.cslice(std::size_t{0}, n);
    auto [ec_send, sent] = co_await qp.async_send(send_buf, use_nothrow);
    if (ec_send) {
      std::cerr << "[server] send error: " << ec_send.message() << "\n";
      break;
    }
  }

  auto [ec_disc] = co_await conn.async_disconnect(use_nothrow);
  (void)ec_disc;
  std::cout << "[server] disconnected\n";
}

asio::awaitable<void> run_client(asio::io_context& io_ctx,
                                 rdma::rdma_device_ptr const& device,
                                 std::string const& host, uint16_t port) {
  std::cout << "[client] connecting to " << host << ":" << port << "\n";

  rdma::rdma_queue_pair qp(io_ctx);
  rdma::rdma_connector<tcp> conn(io_ctx);
  conn.open(qp);

  tcp::endpoint endpoint(asio::ip::make_address(host), port);
  auto [ec_conn] = co_await conn.async_connect(endpoint, {}, use_nothrow);
  if (ec_conn) {
    std::cerr << "[client] connect failed: " << ec_conn.message() << "\n";
    co_return;
  }
  std::cout << "[client] connected\n";

  std::array<char, kBufSize> raw_buf{};
  rdma::rdma_memory_region mr(device, raw_buf.data(), raw_buf.size());

  for (int i = 0; i < kEchoCount; ++i) {
    std::string msg = "Hello RDMA #" + std::to_string(i);
    std::memcpy(raw_buf.data(), msg.data(), msg.size());

    auto send_buf = mr.cslice(std::size_t{0}, msg.size());
    auto [ec_send, sent] = co_await qp.async_send(send_buf, use_nothrow);
    if (ec_send) {
      std::cerr << "[client] send error: " << ec_send.message() << "\n";
      break;
    }

    auto recv_buf = mr.slice(std::size_t{0}, kBufSize);
    auto [ec_recv, n] = co_await qp.async_recv(recv_buf, use_nothrow);
    if (ec_recv) {
      std::cerr << "[client] recv error: " << ec_recv.message() << "\n";
      break;
    }
    std::cout << "[client] echo reply: "
              << std::string_view(raw_buf.data(), n) << "\n";
  }

  auto [ec_disc] = co_await conn.async_disconnect(use_nothrow);
  (void)ec_disc;
  std::cout << "[client] disconnected\n";
}

void print_usage(char const* argv0) {
  std::cerr << "Usage:\n"
            << "  " << argv0 << " --server [--port PORT]\n"
            << "  " << argv0 << " --client HOST [--port PORT]\n";
}

int main(int argc, char* argv[]) {
  bool is_server = false;
  bool is_client = false;
  std::string host;
  uint16_t port = 5000;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--server") {
      is_server = true;
    } else if (arg == "--client" && i + 1 < argc) {
      is_client = true;
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::stoi(argv[++i]));
    }
  }

  if (!is_server && !is_client) {
    print_usage(argv[0]);
    return 1;
  }

  try {
    asio::io_context io_ctx;
    auto& svc = rdma::use_device(io_ctx);
    auto device = svc.get_device();

    if (is_server) {
      asio::co_spawn(io_ctx, run_server(io_ctx, device, port), asio::detached);
    } else {
      asio::co_spawn(io_ctx, run_client(io_ctx, device, host, port),
                     asio::detached);
    }

    io_ctx.run();
    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
