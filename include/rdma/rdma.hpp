#pragma once

// Single umbrella include for the RDMA-on-asio public API. Pulls in the correct
// backend (NetworkDirect on Windows, libibverbs/rdma_cm on Linux) and exposes:
//   - shared value types        (rdma_config_t, rdma_remote_addr_t, mr_acccess_flag_t)
//   - backend-agnostic aliases  (rdma_connector<PS>, rdma_listener<PS>,
//                                rdma_queue_pair, rdma_completion_queue,
//                                rdma_memory_region, rdma_device / rdma_device_ptr)
//   - the tcp port space        (asio::rdma::tcp)
//   - use_device(io_ctx, ...)   device initialization
//
// Include this and write against the rdma_* / tcp::* names for portable code.
#include "rdma/rdma_commons.hpp"
#include "rdma/rdma_error.hpp"
#include "rdma/rdma_types.hpp"
#include "rdma/rdma_address.hpp"
