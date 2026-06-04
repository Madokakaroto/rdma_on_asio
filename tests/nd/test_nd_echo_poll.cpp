// NetworkDirect echo test in POLL MODE — io_context-free data plane.
//
//   - Control plane (connect / accept / disconnect over IND2Connector) runs on an
//     io_context.
//   - Data plane runs against a user-owned nd_completion_queue; the queue pair is
//     bound with nd_queue_pair(cq) (no io_context). A dedicated thread spins
//     cq.poll(); data-plane ops use as_tuple(use_future) and complete inline on
//     the poll thread (the QP uses system_executor in poll mode).
#include <array>
#include <atomic>
#include <future>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <cstring>

#include "asio/io_context.hpp"
#include "asio/use_future.hpp"
#include "asio/as_tuple.hpp"

#include "nd/nd_completion_queue.hpp"
#include "nd/nd_use_device.hpp"
#include "nd/nd_mr.hpp"
#include "rdma/tcp.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

constexpr auto use_fut = asio::as_tuple(asio::use_future);
constexpr std::size_t kBufSize = 4096;
constexpr int kEchoCount = 10;

std::string_view pd_view(asio::const_buffer pd) {
  return {reinterpret_cast<char const*>(pd.data()), pd.size()};
}

// RAII spinner: busy-polls the standalone CQ until destroyed. Construct only
// after the connection is established (the QP exists).
class cq_spinner {
public:
  explicit cq_spinner(rdma::nd_completion_queue& cq)
      : cq_(cq), thread_([this] {
          while (!stop_.load(std::memory_order_relaxed)) {
            cq_.poll();
          }
        }) {}

  ~cq_spinner() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
  }

  cq_spinner(cq_spinner const&) = delete;
  cq_spinner& operator=(cq_spinner const&) = delete;

private:
  rdma::nd_completion_queue& cq_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

void run_server(asio::io_context& io_ctx, rdma::nd_device_ptr const& device,
                uint16_t port) {
  std::cout << "[server] listening on port " << port << " (poll mode)\n";

  rdma::nd_listener<tcp> listener(io_ctx);
  listener.open(tcp::v4());
  listener.bind(tcp::endpoint(asio::ip::address_v4::any(), port));
  listener.listen();

  rdma::nd_completion_queue cq(device);
  rdma::nd_connector<tcp> conn(io_ctx);  // filled by async_get_connection
  rdma::nd_queue_pair qp(cq);            // poll-mode QP: bound to cq, no io

  // --- control plane: get connection (fill form) ---
  asio::error_code get_ec;
  listener.async_get_connection(conn,
                                [&](asio::error_code ec) { get_ec = ec; });
  io_ctx.run();
  io_ctx.restart();
  if (get_ec) {
    std::cerr << "[server] get_connection failed: " << get_ec.message() << "\n";
    return;
  }
  std::cout << "[server] client private data: \""
            << pd_view(conn.get_remote_data()) << "\"\n";

  // --- control plane: accept ---
  asio::error_code accept_ec;
  std::string reply_pd = "server-hello";
  conn.async_accept(qp, asio::buffer(reply_pd),
                    [&](asio::error_code ec) { accept_ec = ec; });
  io_ctx.run();
  io_ctx.restart();
  if (accept_ec) {
    std::cerr << "[server] accept failed: " << accept_ec.message() << "\n";
    return;
  }
  std::cout << "[server] connection accepted\n";

  // --- data plane: poll thread + use_future (io_context-free) ---
  std::array<char, kBufSize> raw_buf{};
  rdma::nd_memory_region mr(device, raw_buf.data(), raw_buf.size());
  {
    cq_spinner spinner(cq);
    for (int i = 0; i < kEchoCount; ++i) {
      auto [ec_recv, n] =
          qp.async_recv(mr.slice(std::size_t{0}, kBufSize), use_fut).get();
      if (ec_recv || n == 0) {
        if (ec_recv && ec_recv != asio::error::operation_aborted) {
          std::cerr << "[server] recv error: " << ec_recv.message() << "\n";
        }
        break;
      }
      std::cout << "[server] echo " << n << " bytes: "
                << std::string_view(raw_buf.data(), n) << "\n";
      auto [ec_send, sent] =
          qp.async_send(mr.cslice(std::size_t{0}, n), use_fut).get();
      if (ec_send) {
        std::cerr << "[server] send error: " << ec_send.message() << "\n";
        break;
      }
    }
  }

  // --- control plane: disconnect ---
  asio::error_code disc_ec;
  conn.async_disconnect([&](asio::error_code ec) { disc_ec = ec; });
  io_ctx.run();
  (void)disc_ec;
  std::cout << "[server] disconnected\n";
}

void run_client(asio::io_context& io_ctx, rdma::nd_device_ptr const& device,
                std::string const& host, uint16_t port) {
  std::cout << "[client] connecting to " << host << ":" << port << " (poll mode)\n";

  rdma::nd_completion_queue cq(device);
  rdma::nd_connector<tcp> conn(io_ctx);
  conn.open(tcp::v4());
  rdma::nd_queue_pair qp(cq);  // poll-mode QP: bound to cq, no io

  // --- control plane: connect ---
  tcp::endpoint endpoint(asio::ip::make_address(host), port);
  std::string req_pd = "client-hello";
  asio::error_code conn_ec;
  conn.async_connect(qp, endpoint, asio::buffer(req_pd),
                     [&](asio::error_code ec) { conn_ec = ec; });
  io_ctx.run();
  io_ctx.restart();
  if (conn_ec) {
    std::cerr << "[client] connect failed: " << conn_ec.message() << "\n";
    return;
  }
  std::cout << "[client] connected; server private data: \""
            << pd_view(conn.get_remote_data()) << "\"\n";

  // --- data plane: poll thread + use_future (io_context-free) ---
  std::array<char, kBufSize> raw_buf{};
  rdma::nd_memory_region mr(device, raw_buf.data(), raw_buf.size());
  {
    cq_spinner spinner(cq);
    for (int i = 0; i < kEchoCount; ++i) {
      std::string msg = "Hello RDMA #" + std::to_string(i);
      std::memcpy(raw_buf.data(), msg.data(), msg.size());

      auto [ec_send, sent] =
          qp.async_send(mr.cslice(std::size_t{0}, msg.size()), use_fut).get();
      if (ec_send) {
        std::cerr << "[client] send error: " << ec_send.message() << "\n";
        break;
      }
      auto [ec_recv, n] =
          qp.async_recv(mr.slice(std::size_t{0}, kBufSize), use_fut).get();
      if (ec_recv) {
        std::cerr << "[client] recv error: " << ec_recv.message() << "\n";
        break;
      }
      std::cout << "[client] echo reply: "
                << std::string_view(raw_buf.data(), n) << "\n";
    }
  }

  // --- control plane: disconnect ---
  asio::error_code disc_ec;
  conn.async_disconnect([&](asio::error_code ec) { disc_ec = ec; });
  io_ctx.run();
  (void)disc_ec;
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
    auto device = rdma::nd_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    rdma::use_device(io_ctx, device);

    if (is_server) {
      run_server(io_ctx, device, port);
    } else {
      run_client(io_ctx, device, host, port);
    }
    return 0;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
