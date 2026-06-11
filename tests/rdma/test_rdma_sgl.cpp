// Scatter/gather data-plane test (cross-platform; rdma_* API only).
//
// Runs server + client coroutines in ONE process, looped back over the RoCE IP.
// The server is a generic "recv-once / echo-once / wait-disconnect" peer; each
// phase's client drives the SGL logic and verifies:
//
//   Phase 1 -- gather-send + scatter-recv across MULTIPLE MRs:
//     the client GATHERs a payload from segments spanning TWO source MRs into a
//     std::vector<rdma_const_buffer> and async_send()s it as one WR; the server
//     recvs it contiguously and echoes it back; the client SCATTER-recvs the
//     echo into TWO destination MRs (std::vector<rdma_mutable_buffer>) and
//     reassembles -> must equal the original payload. Exercises multi-segment +
//     multi-MR SGL in both directions, plus the asio::rdma::buffer() factory.
//
//   Phase 2 -- too-many-sge rejection (Q-C):
//     use_device() with max_send_sge_ = 2; an async_send of a 3-segment sequence
//     is rejected up front with rdma_errc::ext_too_many_sge (a clean library
//     error -- no raw HW EINVAL, no hang), and the connection stays usable (a
//     subsequent single-segment send/echo still round-trips).
//
// Usage: test_rdma_sgl <roce-ip> [port]   (skips if no arg).
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "asio/as_tuple.hpp"
#include "asio/awaitable.hpp"
#include "asio/buffer.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/io_context.hpp"
#include "asio/use_awaitable.hpp"

#include "rdma/rdma.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

// Generic server: accept one connection, recv one message into a contiguous
// buffer, echo those exact bytes back, then wait for the client to disconnect.
asio::awaitable<void> server_echo_once(asio::io_context& io,
                                       rdma::rdma_device_ptr device,
                                       rdma::rdma_listener<tcp>& lis) {
  auto [ecg, conn] = co_await lis.async_get_connection(nothrow);
  if (ecg) co_return;
  rdma::rdma_queue_pair qp(io);
  std::string srv_pd = "s";
  auto [eca] = co_await conn.async_accept(qp, asio::buffer(srv_pd), nothrow);
  if (eca) co_return;

  std::array<char, 256> buf{};
  rdma::rdma_memory_region mr(device, buf.data(), buf.size());
  auto [er, n] = co_await qp.async_recv(mr.slice(std::size_t{0}, buf.size()),
                                        nothrow);
  if (er) co_return;
  auto [es, sn] = co_await qp.async_send(mr.cslice(std::size_t{0}, n), nothrow);
  (void)sn;
  if (es) co_return;
  co_await conn.async_wait_disconnect(nothrow);
}

// ---------------------------------------------------------------------------
// Phase 1: gather-send + scatter-recv across multiple MRs.
// ---------------------------------------------------------------------------
bool phase_gather_scatter(rdma::rdma_device_ptr const& device,
                          std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::use_device(io, device);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(port);
  lis.listen();

  bool ok = false;
  std::atomic<int> remaining{2};
  auto on_done = [&](std::exception_ptr) {
    if (--remaining == 0) io.stop();
  };

  asio::co_spawn(io, server_echo_once(io, device, lis), on_done);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(tcp::v4());
        rdma::rdma_queue_pair qp(io);
        tcp::endpoint ep(asio::ip::make_address(ip), port);
        std::string cli_pd = "c";
        auto [ecc] =
            co_await conn.async_connect(qp, ep, asio::buffer(cli_pd), nothrow);
        if (ecc) co_return;

        // Two SOURCE MRs; the payload is gathered from a segment of each.
        std::string const part_a = "Hello, ";          // 7 bytes
        std::string const part_b = "scatter/gather!";   // 15 bytes
        std::string const payload = part_a + part_b;     // 22 bytes
        std::array<char, 64> a{}, b{};
        std::memcpy(a.data(), part_a.data(), part_a.size());
        std::memcpy(b.data(), part_b.data(), part_b.size());
        rdma::rdma_memory_region mr_a(device, a.data(), a.size());
        rdma::rdma_memory_region mr_b(device, b.data(), b.size());

        // GATHER: one WR, segments from two different MRs (two lkeys).
        std::vector<rdma::rdma_const_buffer> gather{
            mr_a.cslice(std::size_t{0}, part_a.size()),
            mr_b.cslice(std::size_t{0}, part_b.size()),
        };
        auto [es, sn] = co_await qp.async_send(gather, nothrow);
        if (es || sn != payload.size()) co_return;

        // SCATTER: recv the echo into two DESTINATION MRs (built via the
        // asio::rdma::buffer() factory).
        std::array<char, 64> r1{}, r2{};
        rdma::rdma_memory_region mr_r1(device, r1.data(), r1.size());
        rdma::rdma_memory_region mr_r2(device, r2.data(), r2.size());
        std::size_t const n1 = 10, n2 = payload.size() - 10;  // 10 + 12
        std::vector<rdma::rdma_mutable_buffer> scatter{
            rdma::buffer(mr_r1, std::size_t{0}, n1),
            rdma::buffer(mr_r2, std::size_t{0}, n2),
        };
        auto [er, rn] = co_await qp.async_recv(scatter, nothrow);
        if (er || rn != payload.size()) co_return;

        std::string got;
        got.append(r1.data(), n1);
        got.append(r2.data(), n2);
        ok = (got == payload);
        if (!ok) {
          std::cerr << "[phase1] mismatch: got=\"" << got << "\" expected=\""
                    << payload << "\"\n";
        }
        conn.disconnect();
      },
      on_done);

  io.run();

  if (ok) {
    std::cout << "[PASS] phase 1: gather-send + scatter-recv across 2 MRs "
                 "round-trips intact\n";
  } else {
    std::cerr << "[FAIL] phase 1: gather/scatter multi-MR round-trip\n";
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Phase 2: too-many-sge rejected before posting; connection stays usable.
// ---------------------------------------------------------------------------
bool phase_too_many_sge(rdma::rdma_device_ptr const& device,
                        std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::rdma_config_t cfg{};
  cfg.max_send_sge_ = 2;  // cap the send SGL at 2 segments
  rdma::use_device(io, device, cfg);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(tcp::v4());
  lis.bind(port);
  lis.listen();

  bool rejected = false, alive_after = false;
  std::atomic<int> remaining{2};
  auto on_done = [&](std::exception_ptr) {
    if (--remaining == 0) io.stop();
  };

  asio::co_spawn(io, server_echo_once(io, device, lis), on_done);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(tcp::v4());
        rdma::rdma_queue_pair qp(io);
        tcp::endpoint ep(asio::ip::make_address(ip), port);
        std::string cli_pd = "c";
        auto [ecc] =
            co_await conn.async_connect(qp, ep, asio::buffer(cli_pd), nothrow);
        if (ecc) co_return;

        std::array<char, 64> buf{};
        std::memcpy(buf.data(), "0123456789ab", 12);
        rdma::rdma_memory_region mr(device, buf.data(), buf.size());

        // 3 segments > max_send_sge_(2) -> rejected up front, no wire traffic.
        std::vector<rdma::rdma_const_buffer> too_many{
            mr.cslice(std::size_t{0}, 4),
            mr.cslice(std::size_t{4}, 4),
            mr.cslice(std::size_t{8}, 4),
        };
        auto [es, sn] = co_await qp.async_send(too_many, nothrow);
        (void)sn;
        rejected = (es == rdma::rdma_errc::ext_too_many_sge);
        if (!rejected) {
          std::cerr << "[phase2] expected ext_too_many_sge, got: "
                    << es.message() << "\n";
        }

        // Connection still usable: a valid single-segment send/echo round-trips.
        auto [es2, sn2] =
            co_await qp.async_send(mr.cslice(std::size_t{0}, 12), nothrow);
        if (es2 || sn2 != 12) {
          conn.disconnect();
          co_return;
        }
        std::array<char, 64> rbuf{};
        rdma::rdma_memory_region rmr(device, rbuf.data(), rbuf.size());
        auto [er, rn] =
            co_await qp.async_recv(rmr.slice(std::size_t{0}, rbuf.size()),
                                   nothrow);
        alive_after = (!er && rn == 12 &&
                       std::string_view(rbuf.data(), 12) == "0123456789ab");
        conn.disconnect();
      },
      on_done);

  io.run();

  bool const ok = rejected && alive_after;
  if (ok) {
    std::cout << "[PASS] phase 2: 3-segment send rejected with "
                 "ext_too_many_sge; connection still usable\n";
  } else {
    std::cerr << "[FAIL] phase 2: rejected=" << rejected
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
  uint16_t port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 5060;

  try {
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device(tcp::v4(), {});
    bool ok = true;
    ok &= phase_gather_scatter(device, ip, port);
    ok &= phase_too_many_sge(device, ip, static_cast<uint16_t>(port + 1));

    if (ok) {
      std::cout << "\nAll rdma scatter/gather tests passed.\n";
      return 0;
    }
    std::cerr << "\n[FAIL] one or more phases failed.\n";
    return 1;
  } catch (std::exception const& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
