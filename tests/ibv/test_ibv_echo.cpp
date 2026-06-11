#include <array>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <cstring>

#include "asio/io_context.hpp"
#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/use_awaitable.hpp"
#include "asio/as_tuple.hpp"

#include "ibv/ibv_use_device.hpp"
#include "ibv/ibv_mr.hpp"
#include "rdma/tcp.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

constexpr auto use_nothrow = asio::as_tuple(asio::use_awaitable);
constexpr std::size_t kBufSize = 4096;
constexpr int kEchoCount = 10;

std::string_view pd_view(asio::const_buffer pd) {
  return {reinterpret_cast<char const*>(pd.data()), pd.size()};
}

asio::awaitable<void> run_server(asio::io_context& io_ctx,
                                 rdma::ibv_device_ptr const& device,
                                 uint16_t port) {
  std::cout << "[server] listening on port " << port << "\n";

  rdma::ibv_listener<tcp> listener(io_ctx);
  listener.open(tcp::v4());
  listener.bind(port);
  listener.listen();

  std::array<char, 256> req_pd_buf{};
  auto [ec_get, conn, req_pd_len] = co_await listener.async_get_connection(
      asio::buffer(req_pd_buf), use_nothrow);
  if (ec_get) {
    std::cerr << "[server] get_connection failed: " << ec_get.message() << "\n";
    co_return;
  }
  std::cout << "[server] client private data: \""
            << pd_view(asio::buffer(req_pd_buf.data(), req_pd_len)) << "\"\n";

  rdma::ibv_queue_pair qp(io_ctx);
  std::string reply_pd = "server-hello";
  auto [ec_accept] = co_await conn.async_accept(qp, asio::buffer(reply_pd), use_nothrow);
  if (ec_accept) {
    std::cerr << "[server] accept failed: " << ec_accept.message() << "\n";
    co_return;
  }
  std::cout << "[server] connection accepted\n";

  std::array<char, kBufSize> raw_buf{};
  rdma::ibv_memory_region mr(device, raw_buf.data(), raw_buf.size());

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

  asio::error_code ec_disc;
  conn.disconnect(ec_disc);
  (void)ec_disc;
  std::cout << "[server] disconnected\n";
}

asio::awaitable<void> run_client(asio::io_context& io_ctx,
                                 rdma::ibv_device_ptr const& device,
                                 std::string const& host, uint16_t port) {
  std::cout << "[client] connecting to " << host << ":" << port << "\n";

  rdma::ibv_connector<tcp> conn(io_ctx);
  conn.open(tcp::v4());
  rdma::ibv_queue_pair qp(io_ctx);

  tcp::endpoint endpoint(asio::ip::make_address(host), port);
  std::string req_pd = "client-hello";
  std::array<char, 256> reply_pd_buf{};
  auto [ec_conn, reply_pd_len] = co_await conn.async_connect(
      qp, endpoint, asio::buffer(req_pd), asio::buffer(reply_pd_buf),
      use_nothrow);
  if (ec_conn) {
    std::cerr << "[client] connect failed: " << ec_conn.message() << "\n";
    co_return;
  }
  std::cout << "[client] connected; server private data: \""
            << pd_view(asio::buffer(reply_pd_buf.data(), reply_pd_len)) << "\"\n";

  std::array<char, kBufSize> raw_buf{};
  rdma::ibv_memory_region mr(device, raw_buf.data(), raw_buf.size());

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

  asio::error_code ec_disc;
  conn.disconnect(ec_disc);
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
    auto device = rdma::ibv_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io_ctx, device);

    // Event mode keeps the shared-CQ poller armed for the io_context's lifetime,
    // so io.run() no longer returns on idle — stop it when the echo finishes.
    auto on_done = [&io_ctx](std::exception_ptr) { io_ctx.stop(); };
    if (is_server) {
      asio::co_spawn(io_ctx, run_server(io_ctx, device, port), on_done);
    } else {
      asio::co_spawn(io_ctx, run_client(io_ctx, device, host, port), on_done);
    }

    io_ctx.run();
    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
