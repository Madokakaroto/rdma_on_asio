#pragma once

#include <cstdint>

// Backend-independent RDMA types shared by the nd (Windows NetworkDirect) and
// ibv (Linux libibverbs) backends. Only one backend compiles per platform, but
// these definitions are identical for both, so they live in one place. Each
// backend aliases its prefixed names (nd_config_t / ibv_config_t, etc.) to
// these for source compatibility.
namespace asio::rdma {

// command types
enum mr_acccess_flag_t {
  mr_access_local_write,
  mr_access_remote_read,
  mr_access_remote_write,
};

// How a queue pair's completions are delivered (see queue_pair::bound_type()).
// Mirrors RDMA's two completion-notification mechanisms: polling a plain CQ
// (poll) vs. completion-channel / IOCP event notification (event).
enum class completion_mode {
  none,    // not bound to any completion mechanism yet
  event,   // event-driven: io_context's managed CQ, completion-channel/IOCP notification
  poll,    // polled: a user-owned completion_queue, reaped via poll()/poll_one()
};

// configuration to initialize the shared state.
// 0 = auto-derive from device capabilities using min(device_max, default).
struct rdma_config_t {
  // CQ configuration
  std::uint32_t cqe_ = 0;                // 0 = min(device_max, 4096)

  // QP configuration
  std::uint32_t max_send_wr_ = 0;        // 0 = min(device_max, 128)
  std::uint32_t max_recv_wr_ = 0;        // 0 = min(device_max, 128)
  std::uint32_t max_send_sge_ = 0;       // 0 = min(device_max, 4)
  std::uint32_t max_recv_sge_ = 0;       // 0 = min(device_max, 4)
  std::uint32_t max_inline_data_ = 0;    // 0 = device default

  // Connection configuration
  std::uint32_t inbound_read_limit_ = 0;  // 0 = device default
  std::uint32_t outbound_read_limit_ = 0; // 0 = device default
  // CM address/route resolution timeout (ms). 0 = default_cm_resolve_timeout_ms.
  // ibv only: feeds rdma_resolve_addr / rdma_resolve_route. nd ignores it --
  // ND's IND2Connector::Connect resolves internally with no exposed timeout.
  std::uint32_t cm_resolve_timeout_ms_ = 0;

  // Listener configuration
  int backlog_ = 128;
};

// Default CM resolve timeout (ms) used when cm_resolve_timeout_ms_ == 0.
inline constexpr std::uint32_t default_cm_resolve_timeout_ms = 2000;

// Remote memory region handle (address + remote key/token) for RDMA read/write.
struct rdma_remote_addr_t {
  std::uint64_t addr_;
  std::uint32_t token_;
};

namespace detail {
// buffer kind tags used by the buffer concepts and the MR buffer types
struct rdma_const_buffer_tag {};
struct rdma_mutable_buffer_tag {};
}

}
