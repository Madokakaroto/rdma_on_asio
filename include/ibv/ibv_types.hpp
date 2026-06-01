#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rdma/rdma_types.hpp"

namespace asio::rdma {

using size_type = std::uint32_t;
using result_type = int;  // verbs return code (0 = ok, non-zero = error)

// mr_acccess_flag_t is shared (rdma/rdma_types.hpp).

// Backend config/remote-addr are backend-independent; alias the shared types.
using ibv_config_t = rdma_config_t;
using ibv_remote_addr_t = rdma_remote_addr_t;  // token_ = rkey

}

// types not used directly
#include "ibv/detail/ibv_impl_types.hpp"
