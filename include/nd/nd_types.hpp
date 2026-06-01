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

#include "rdma/rdma_types.hpp"

namespace asio::rdma {

using size_type = ULONG;
using result_type = HRESULT;

// mr_acccess_flag_t is shared (rdma/rdma_types.hpp).

// Backend config/remote-addr are backend-independent; alias the shared types.
using nd_config_t = rdma_config_t;
using nd_remote_addr_t = rdma_remote_addr_t;

}

// types not used directly
#include "nd/detail/nd_impl_types.hpp"