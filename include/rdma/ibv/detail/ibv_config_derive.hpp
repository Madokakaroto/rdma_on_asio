#pragma once

#include <algorithm>

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "rdma/ibv/ibv_types.hpp"
#include "rdma/ibv/detail/ibv_impl_types.hpp"

namespace asio::rdma::detail {

inline constexpr size_type default_cqe = 4096;
inline constexpr size_type default_cq_poll_batch = 16;
inline constexpr size_type default_max_send_wr = 128;
inline constexpr size_type default_max_recv_wr = 128;
inline constexpr size_type default_max_send_sge = 4;
inline constexpr size_type default_max_recv_sge = 4;

// ibv_device_attr cap fields are signed int; cast to size_type before use.
inline size_type cap_of(int v) {
  return v > 0 ? static_cast<size_type>(v) : 0u;
}

ASIO_DECL ibv_config_t derive_effective_config(ibv_config_t const& user_config,
                                               native_device_attr_t const& caps);

ASIO_DECL bool is_config_compatible(ibv_config_t const& config,
                                    native_device_attr_t const& caps);

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/detail/impl/ibv_config_derive.ipp"
#endif
