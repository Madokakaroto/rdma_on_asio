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
//     is rejected up front with rdma_errc::too_many_sge (a clean library
//     error -- no raw HW EINVAL, no hang), and the connection stays usable (a
//     subsequent single-segment send/echo still round-trips).
//
//   Phase 3 -- 9-segment gather/scatter heap spill (send/recv):
//     a 9-segment SGL exceeds small_sglist's inline_count (8), so the native SGE
//     list spills to a heap buffer. This exercises the no-TLS heap-spill path on
//     the wire (unit tests only cover the logic) for both gather-send and
//     scatter-recv. Requires max_*_sge_ >= 9 (the default derives to 4).
//
//   Phase 4 -- 9-segment heap spill (one-sided read/write):
//     the one-sided counterpart to Phase 3 -- a 9-SGE RDMA read (scatter) and a
//     9-SGE RDMA write (gather) against a remote MR. do_post_read/write share the
//     same build_native_sglist + small_sglist machinery as send/recv, so this
//     validates the heap-spill SGL for the one-sided verbs too.
//
// Usage: test_rdma_sgl [port].
// The test queries a local RDMA-capable address at runtime.
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
#include "rdma_test_address.hpp"

namespace rdma = asio::rdma;
using tcp = rdma::tcp;

constexpr auto nothrow = asio::as_tuple(asio::use_awaitable);

// Generic server: accept one connection, recv one message into a contiguous
// buffer, echo those exact bytes back, then wait for the client to disconnect.
asio::awaitable<void> server_echo_once(asio::io_context& io,
                                       rdma::rdma_device_ptr device,
                                       rdma::rdma_listener<tcp>& lis) {
  auto [ecg, conn, rqn] =
      co_await lis.async_get_connection(asio::mutable_buffer{}, nothrow);
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
  lis.open(rdma_test::port_space_for(ip));
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
        conn.open(rdma_test::port_space_for(ip));
        rdma::rdma_queue_pair qp(io);
        auto ep = rdma_test::endpoint_for(ip, port);
        std::string cli_pd = "c";
        auto [ecc, rpn] = co_await conn.async_connect(
            qp, ep, asio::buffer(cli_pd), asio::mutable_buffer{}, nothrow);
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
  lis.open(rdma_test::port_space_for(ip));
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
        conn.open(rdma_test::port_space_for(ip));
        rdma::rdma_queue_pair qp(io);
        auto ep = rdma_test::endpoint_for(ip, port);
        std::string cli_pd = "c";
        auto [ecc, rpn] = co_await conn.async_connect(
            qp, ep, asio::buffer(cli_pd), asio::mutable_buffer{}, nothrow);
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
        rejected = (es == rdma::rdma_errc::too_many_sge);
        if (!rejected) {
          std::cerr << "[phase2] expected too_many_sge, got: "
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
                 "too_many_sge; connection still usable\n";
  } else {
    std::cerr << "[FAIL] phase 2: rejected=" << rejected
              << " alive_after=" << alive_after << "\n";
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Phase 3: 9-segment gather/scatter -- exceeds small_sglist inline_count (8), so
// the native SGE list spills to heap. Unit tests cover the heap-spill logic; this
// exercises it on the wire (RoCE) in both gather-send and scatter-recv. The
// read/write multi-SGE paths share the same build_native_sglist + small_sglist
// machinery (do_post_read/write differ only in opcode + remote address), so this
// + the Phase 4 single-buffer read/write perftest runs cover them.
// ---------------------------------------------------------------------------
bool phase_heap_spill(rdma::rdma_device_ptr const& device, std::string const& ip,
                      uint16_t port) {
  asio::io_context io;
  // The effective max_*_sge defaults to min(device_cap, 4); raise it so a
  // 9-segment SGL (> small_sglist inline_count 8) is accepted and actually
  // posts -- this is what exercises the heap-spill path on the wire.
  rdma::rdma_config_t cfg{};
  cfg.max_send_sge_ = 9;
  cfg.max_recv_sge_ = 9;
  rdma::use_device(io, device, cfg);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(rdma_test::port_space_for(ip));
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
        conn.open(rdma_test::port_space_for(ip));
        rdma::rdma_queue_pair qp(io);
        auto ep = rdma_test::endpoint_for(ip, port);
        std::string cli_pd = "c";
        auto [ecc, rpn] = co_await conn.async_connect(
            qp, ep, asio::buffer(cli_pd), asio::mutable_buffer{}, nothrow);
        if (ecc) co_return;

        // 9 contiguous segments (> inline_count 8) -> the SGE list spills to heap.
        constexpr std::size_t kSeg = 9;
        constexpr std::size_t kSegLen = 4;
        constexpr std::size_t kTotal = kSeg * kSegLen;  // 36 bytes
        std::array<char, 64> src{};
        for (std::size_t i = 0; i < kTotal; ++i) {
          src[i] = static_cast<char>('A' + (i % 26));
        }
        rdma::rdma_memory_region mr_src(device, src.data(), src.size());

        std::vector<rdma::rdma_const_buffer> gather;
        for (std::size_t i = 0; i < kSeg; ++i) {
          gather.push_back(mr_src.cslice(i * kSegLen, kSegLen));
        }
        auto [es, sn] = co_await qp.async_send(gather, nothrow);
        if (es || sn != kTotal) {
          std::cerr << "[phase3] 9-SGE gather-send failed: ec='" << es.message()
                    << "' sn=" << sn << " (expect " << kTotal << ")\n";
          conn.disconnect();
          co_return;
        }

        std::array<char, 64> dst{};
        rdma::rdma_memory_region mr_dst(device, dst.data(), dst.size());
        std::vector<rdma::rdma_mutable_buffer> scatter;
        for (std::size_t i = 0; i < kSeg; ++i) {
          scatter.push_back(rdma::buffer(mr_dst, i * kSegLen, kSegLen));
        }
        auto [er, rn] = co_await qp.async_recv(scatter, nothrow);
        if (er || rn != kTotal) {
          std::cerr << "[phase3] 9-SGE scatter-recv failed: ec='" << er.message()
                    << "' rn=" << rn << "\n";
          conn.disconnect();
          co_return;
        }

        ok = std::memcmp(src.data(), dst.data(), kTotal) == 0;
        if (!ok) {
          std::cerr << "[phase3] 9-SGE heap-spill round-trip mismatch\n";
        }
        conn.disconnect();
      },
      on_done);

  io.run();

  if (ok) {
    std::cout << "[PASS] phase 3: 9-SGE gather/scatter (heap spill > inline 8) "
                 "round-trips intact\n";
  } else {
    std::cerr << "[FAIL] phase 3: 9-SGE heap-spill round-trip\n";
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Phase 4: 9-segment (heap-spill) RDMA write + read against a remote MR. This is
// the one-sided counterpart to Phase 3: do_post_read/do_post_write share the
// exact same build_native_sglist + small_sglist machinery as send/recv (they
// differ only in opcode + remote address), so this validates the heap-spill SGL
// on the wire for the one-sided verbs too. Single-process loopback: the client
// reads the server MR's remote_addr directly (shared variable, no wire exchange).
// ---------------------------------------------------------------------------
bool phase_rw_heap_spill(rdma::rdma_device_ptr const& device,
                         std::string const& ip, uint16_t port) {
  asio::io_context io;
  rdma::rdma_config_t cfg{};
  cfg.max_send_sge_ = 9;  // RDMA read/write use the send SGL
  cfg.max_recv_sge_ = 9;
  rdma::use_device(io, device, cfg);

  rdma::rdma_listener<tcp> lis(io);
  lis.open(rdma_test::port_space_for(ip));
  lis.bind(port);
  lis.listen();

  constexpr std::size_t kSeg = 9;
  constexpr std::size_t kSegLen = 4;
  constexpr std::size_t kTotal = kSeg * kSegLen;  // 36 bytes
  constexpr std::size_t kWriteOff = 128;

  // Server MR: known readable pattern at [0, kTotal); the client RDMA-writes
  // into [kWriteOff, kWriteOff + kTotal).
  std::array<char, 256> srv_storage{};
  for (std::size_t i = 0; i < kTotal; ++i) {
    srv_storage[i] = static_cast<char>('a' + (i % 26));
  }
  rdma::rdma_memory_region srv_mr(device, srv_storage.data(), srv_storage.size());
  auto const read_remote = srv_mr.remote_addr(std::size_t{0}, kTotal);
  auto const write_remote = srv_mr.remote_addr(kWriteOff, kTotal);

  bool read_ok = false, write_ok = false;
  std::atomic<int> remaining{2};
  auto on_done = [&](std::exception_ptr) {
    if (--remaining == 0) io.stop();
  };

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto [ecg, conn, rqn] =
            co_await lis.async_get_connection(asio::mutable_buffer{}, nothrow);
        if (ecg) co_return;
        rdma::rdma_queue_pair qp(io);
        auto [eca] = co_await conn.async_accept(qp, asio::const_buffer{},
                                                nothrow);
        if (eca) co_return;

        // One control byte signals "client write complete" -> verify the write.
        std::array<char, 1> ctrl{};
        rdma::rdma_memory_region ctrl_mr(device, ctrl.data(), ctrl.size());
        auto [er, n] =
            co_await qp.async_recv(ctrl_mr.slice(std::size_t{0}, ctrl.size()),
                                   nothrow);
        write_ok = !er && n == 1;
        for (std::size_t i = 0; i < kTotal && write_ok; ++i) {
          write_ok = srv_storage[kWriteOff + i] ==
                     static_cast<char>('A' + (i % 26));
        }
        if (!write_ok) {
          std::cerr << "[phase4] server write verification failed: ec='"
                    << er.message() << "' n=" << n << "\n";
        }
        co_await conn.async_wait_disconnect(nothrow);
      },
      on_done);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        rdma::rdma_connector<tcp> conn(io);
        conn.open(rdma_test::port_space_for(ip));
        rdma::rdma_queue_pair qp(io);
        auto ep = rdma_test::endpoint_for(ip, port);
        auto [ecc, rpn] = co_await conn.async_connect(
            qp, ep, asio::const_buffer{}, asio::mutable_buffer{}, nothrow);
        if (ecc) co_return;

        std::array<char, 256> cli_storage{};
        rdma::rdma_memory_region cli_mr(device, cli_storage.data(),
                                        cli_storage.size());

        // RDMA READ into 9 scatter SGEs (heap spill) <- contiguous remote region.
        std::vector<rdma::rdma_mutable_buffer> scatter;
        for (std::size_t i = 0; i < kSeg; ++i) {
          scatter.push_back(rdma::buffer(cli_mr, i * kSegLen, kSegLen));
        }
        auto [erd, rdn] = co_await qp.async_read(scatter, read_remote, nothrow);
        read_ok = !erd && rdn == kTotal;
        for (std::size_t i = 0; i < kTotal && read_ok; ++i) {
          read_ok = cli_storage[i] == static_cast<char>('a' + (i % 26));
        }
        if (!read_ok) {
          std::cerr << "[phase4] 9-SGE read failed: ec='" << erd.message()
                    << "' rn=" << rdn << "\n";
          conn.disconnect();
          co_return;
        }

        // RDMA WRITE from 9 gather SGEs (heap spill) -> contiguous remote region.
        for (std::size_t i = 0; i < kTotal; ++i) {
          cli_storage[64 + i] = static_cast<char>('A' + (i % 26));
        }
        std::vector<rdma::rdma_const_buffer> gather;
        for (std::size_t i = 0; i < kSeg; ++i) {
          gather.push_back(cli_mr.cslice(64 + i * kSegLen, kSegLen));
        }
        auto [ewr, wrn] = co_await qp.async_write(gather, write_remote, nothrow);
        if (ewr || wrn != kTotal) {
          std::cerr << "[phase4] 9-SGE write failed: ec='" << ewr.message()
                    << "' wn=" << wrn << "\n";
          conn.disconnect();
          co_return;
        }

        // Signal the server that the write landed (1-byte control send).
        cli_storage[200] = '!';
        co_await qp.async_send(cli_mr.cslice(std::size_t{200}, std::size_t{1}),
                               nothrow);
        conn.disconnect();
      },
      on_done);

  io.run();

  bool const ok = read_ok && write_ok;
  if (ok) {
    std::cout << "[PASS] phase 4: 9-SGE RDMA read + write (heap spill > inline 8) "
                 "round-trips intact\n";
  } else {
    std::cerr << "[FAIL] phase 4: read_ok=" << read_ok
              << " write_ok=" << write_ok << "\n";
  }
  return ok;
}

int main(int argc, char* argv[]) {
  try {
    auto endpoint =
        rdma_test::query_local_endpoint_with_port_arg(argc, argv, 5060);
    auto ip = endpoint.address_string();
    auto port = endpoint.port;
    auto device = rdma::rdma_device_manager_t::instance()
                      .get_first_available_device({});
    bool ok = true;
    ok &= phase_gather_scatter(device, ip, port);
    ok &= phase_too_many_sge(device, ip, static_cast<uint16_t>(port + 1));
    ok &= phase_heap_spill(device, ip, static_cast<uint16_t>(port + 2));
    ok &= phase_rw_heap_spill(device, ip, static_cast<uint16_t>(port + 3));

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
