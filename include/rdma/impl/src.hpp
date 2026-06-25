//
// rdma/impl/src.hpp
// ~~~~~~~~~~~~~~~~~
//
// Single source-umbrella for separate compilation of rdma_on_asio.
//
// Usage (consumer side, mirrors asio):
//   1. Define ASIO_SEPARATE_COMPILATION for ALL translation units (build flag).
//   2. In exactly ONE .cpp of your program:  #include <rdma/impl/src.hpp>
//
// That single include also pulls in asio's own implementation (asio/impl/src.hpp),
// so you do NOT need a separate asio source TU and must NOT #define ASIO_SOURCE
// yourself -- asio/impl/src.hpp does that. The library ships only this header;
// the one-line .cpp that includes it is written by the consumer (or, in this
// repo, by the tests). See docs/separate_compilation_usage.md.
//
#ifndef RDMA_IMPL_SRC_HPP
#define RDMA_IMPL_SRC_HPP

#include "asio/detail/config.hpp"

#if defined(ASIO_HEADER_ONLY)
# error Do not compile rdma_on_asio library source with ASIO_HEADER_ONLY defined
#endif

// Compile asio's implementation first. This defines ASIO_SOURCE (so ASIO_DECL
// resolves to dllexport under ASIO_DYN_LINK) and must not be re-defined here.
#include "asio/impl/src.hpp"

// Make all backend-agnostic declarations (and the active backend selection) visible.
#include "rdma/rdma.hpp"

// --- shared layer .ipp ---
#include "rdma/impl/rdma_error.ipp"

// --- active-backend .ipp (gated by the backend macro set in tcp.hpp) ---
#if defined(ASIO_RDMA_BACKEND_VERBS)
#  include "rdma/ibv/detail/impl/ibv_config_derive.ipp"
#  include "rdma/ibv/detail/impl/ibv_device_impl.ipp"
#  include "rdma/ibv/detail/impl/ibv_ops_cm.ipp"
#  include "rdma/ibv/detail/impl/ibv_ops_verbs.ipp"
#  include "rdma/ibv/detail/impl/ibv_op_complete.ipp"
#  include "rdma/ibv/detail/impl/ibv_op_cm.ipp"
#  include "rdma/ibv/detail/impl/ibv_op_connect.ipp"
#  include "rdma/ibv/detail/impl/ibv_op_wait_disconnect.ipp"
#  include "rdma/ibv/detail/impl/ibv_service_base.ipp"
#  include "rdma/ibv/detail/impl/ibv_service_io_completion.ipp"
#  include "rdma/ibv/detail/impl/ibv_service_verbs.ipp"
#  include "rdma/ibv/impl/ibv_device.ipp"
#  include "rdma/ibv/impl/ibv_completion_queue.ipp"
#  include "rdma/ibv/impl/ibv_use_device.ipp"
#  include "rdma/ibv/impl/ibv_queue_pair.ipp"
#elif defined(ASIO_RDMA_BACKEND_ND)
#  include "rdma/nd/detail/impl/nd_config_derive.ipp"
#  include "rdma/nd/detail/impl/nd_device_impl.ipp"
#  include "rdma/nd/detail/impl/nd_ops_verbs.ipp"
#  include "rdma/nd/detail/impl/nd_ops_cm.ipp"
#  include "rdma/nd/detail/impl/nd_op_base.ipp"
#  include "rdma/nd/detail/impl/nd_op_complete.ipp"
#  include "rdma/nd/detail/impl/nd_op_disconnect.ipp"
#  include "rdma/nd/detail/impl/nd_op_connect.ipp"
#  include "rdma/nd/detail/impl/nd_service_base.ipp"
#  include "rdma/nd/detail/impl/nd_service_io_completion.ipp"
#  include "rdma/nd/detail/impl/nd_service_verbs.ipp"
#  include "rdma/nd/detail/impl/nd_asio_manual_init.ipp"
#  include "rdma/nd/impl/nd_error.ipp"
#  include "rdma/nd/impl/nd_device.ipp"
#  include "rdma/nd/impl/nd_completion_queue.ipp"
#  include "rdma/nd/impl/nd_mr.ipp"
#  include "rdma/nd/impl/nd_use_device.ipp"
#endif

#endif  // RDMA_IMPL_SRC_HPP
