#pragma once

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "rdma/nd/nd_types.hpp"
#include "rdma/nd/detail/nd_impl_types.hpp"

namespace asio::rdma::detail {

inline constexpr size_type default_cqe = 4096;
inline constexpr size_type default_cq_poll_batch = 16;
inline constexpr size_type default_max_send_wr = 128;
inline constexpr size_type default_max_recv_wr = 128;
inline constexpr size_type default_max_send_sge = 4;
inline constexpr size_type default_max_recv_sge = 4;

ASIO_DECL nd_config_t derive_effective_config(nd_config_t const& user_config,
                                              native_context_config_t const& caps);

ASIO_DECL bool is_config_compatible(nd_config_t const& config,
                                    native_context_config_t const& caps);

// Device selection additionally checks that an adapter object is usable.

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/detail/impl/nd_config_derive.ipp"
#endif
