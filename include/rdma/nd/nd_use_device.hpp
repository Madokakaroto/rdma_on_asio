#pragma once

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/io_context.hpp"
#include "rdma/nd/nd_device.hpp"
#include "rdma/nd/detail/nd_config_derive.hpp"
#include "rdma/nd/detail/nd_service_device.hpp"
#include "rdma/nd/detail/nd_service_io_completion.hpp"

namespace asio::rdma {

// Initialize the per-io_context shared-CQ service for an explicit device. The
// caller discovers the device beforehand via
// nd_device_manager_t::instance().get_first_available_device(config).
//
// Returns void: the caller already holds the device_ptr. The same device_ptr may
// be passed to use_device on multiple io_contexts. Mirrors ibv use_device.
ASIO_DECL void use_device(asio::io_context& io_ctx, nd_device_ptr const& device,
                          nd_config_t const& config, asio::error_code& ec);

ASIO_DECL void use_device(asio::io_context& io_ctx, nd_device_ptr const& device,
                          nd_config_t const& config = {});

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/nd/impl/nd_use_device.ipp"
#endif
