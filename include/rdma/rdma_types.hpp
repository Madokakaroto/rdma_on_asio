#pragma once

// Backend-agnostic public type aliases. Selects the active backend's concrete
// types (nd_* on Windows, ibv_* on Linux) behind a single set of rdma_* names,
// so application code can be written portably (further reduced via rdma.hpp).
//
// tcp.hpp sets ASIO_RDMA_BACKEND_{ND,VERBS} and pulls in the (port-space-bound)
// connector/listener; here we add the remaining backend headers — including the
// port-space-agnostic queue_pair — and define the aliases.
#include "rdma/tcp.hpp"

#if defined(ASIO_RDMA_BACKEND_ND)
#  include "nd/nd_queue_pair.hpp"
#  include "nd/nd_completion_queue.hpp"
#  include "nd/nd_mr.hpp"
#  include "nd/nd_device.hpp"
#  include "nd/nd_use_device.hpp"
#elif defined(ASIO_RDMA_BACKEND_VERBS)
#  include "ibv/ibv_queue_pair.hpp"
#  include "ibv/ibv_completion_queue.hpp"
#  include "ibv/ibv_mr.hpp"
#  include "ibv/ibv_device.hpp"
#  include "ibv/ibv_use_device.hpp"
#endif

namespace asio::rdma {

#if defined(ASIO_RDMA_BACKEND_ND)

// connector/listener remain templated on the port space.
template <typename PortSpace>
using rdma_connector = nd_connector<PortSpace>;
template <typename PortSpace>
using rdma_listener = nd_listener<PortSpace>;

using rdma_queue_pair = nd_queue_pair;
using rdma_completion_queue = nd_completion_queue;
using rdma_memory_region = nd_memory_region;
using rdma_device = nd_device_t;
using rdma_device_ptr = nd_device_ptr;

#elif defined(ASIO_RDMA_BACKEND_VERBS)

template <typename PortSpace>
using rdma_connector = ibv_connector<PortSpace>;
template <typename PortSpace>
using rdma_listener = ibv_listener<PortSpace>;

using rdma_queue_pair = ibv_queue_pair;
using rdma_completion_queue = ibv_completion_queue;
using rdma_memory_region = ibv_memory_region;
using rdma_device = ibv_device_t;
using rdma_device_ptr = ibv_device_ptr;

#endif

}
