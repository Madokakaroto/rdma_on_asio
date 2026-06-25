#pragma once

#include "asio/detail/config.hpp"  // ASIO_DECL / ASIO_HEADER_ONLY
#include "asio/io_context.hpp"
#include "rdma/ibv/ibv_device.hpp"
#include "rdma/ibv/ibv_error.hpp"
#include "rdma/ibv/detail/ibv_config_derive.hpp"
#include "rdma/ibv/detail/ibv_service_device.hpp"
#include "rdma/ibv/detail/ibv_service_io_completion.hpp"

namespace asio::rdma {

// Initialize the per-io_context shared-CQ service for an explicit device. The
// caller discovers the device beforehand via
// ibv_device_manager_t::instance().get_first_available_device(config).
//
// Returns void: the caller already holds the device_ptr (use it directly for
// MR / completion_queue / etc.). The same device_ptr may be passed to use_device
// on multiple io_contexts --each gets its own shared CQ + comp_channel bound to
// its own reactor, all sharing the device's context/PD. Mirrors nd use_device.
ASIO_DECL void use_device(asio::io_context& io_ctx, ibv_device_ptr const& device,
                          ibv_config_t const& config, asio::error_code& ec);

ASIO_DECL void use_device(asio::io_context& io_ctx, ibv_device_ptr const& device,
                          ibv_config_t const& config = {});

}

#if defined(ASIO_HEADER_ONLY)
# include "rdma/ibv/impl/ibv_use_device.ipp"
#endif
