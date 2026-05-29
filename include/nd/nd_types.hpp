#pragma once

#include <memory>
#include <string>
#include <vector>
#include <array>
#include <numeric>
#include <ranges>
#include <iterator>
#include <winnt.h>
#include <wrl/client.h>
#include <libloaderapi.h>
#include <ws2spi.h>
#include <guiddef.h>
#include <ndsupport.h>
#include <ndstatus.h>
#include <ndspi.h>

namespace asio::rdma {

using size_type = ULONG;
using result_type = HRESULT;

// command types
enum mr_acccess_flag_t {
  mr_access_local_write,
  mr_access_remote_read,
  mr_access_remote_write,
};

// configuration type to initialize the shared state
// 0 = auto-derive from device capabilities using min(device_max, reasonable_default)
struct nd_config_t {
  // CQ configuration (used by nd_io_completion_service / nd_completion_queue)
  size_type cqe_ = 0;                // 0 = min(device_max, 4096)

  // QP configuration (used by nd_queue_pair open)
  size_type max_send_wr_ = 0;        // 0 = min(device_max, 128)
  size_type max_recv_wr_ = 0;        // 0 = min(device_max, 128)
  size_type max_send_sge_ = 0;       // 0 = min(device_max, 4)
  size_type max_recv_sge_ = 0;       // 0 = min(device_max, 4)
  size_type max_inline_data_ = 0;    // 0 = device default

  // Connection configuration
  size_type inbound_read_limit_ = 0;  // 0 = device default
  size_type outbound_read_limit_ = 0; // 0 = device default

  // Listener configuration
  int backlog_ = 128;
};

struct nd_remote_addr_t {
  std::uint64_t addr_;
  std::uint32_t token_;
};

}

// types not used directly
#include "nd/detail/nd_impl_types.hpp"