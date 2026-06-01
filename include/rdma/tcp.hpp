#pragma once

#include "asio/ip/tcp.hpp"

#if defined(_WIN32) || defined(_WIN64)
#  include "nd/nd_queue_pair.hpp"
#  include "nd/nd_connector.hpp"
#  include "nd/nd_listener.hpp"
#  define ASIO_RDMA_BACKEND_ND 1
#elif defined(__linux__)
#  include <rdma/rdma_cma.h>
#  include "ibv/ibv_queue_pair.hpp"
#  include "ibv/ibv_connector.hpp"
#  include "ibv/ibv_listener.hpp"
#  define ASIO_RDMA_BACKEND_VERBS 1
#else
#  error "Unsupported RDMA platform"
#endif

namespace asio::rdma {

class tcp {
public:
  using endpoint = asio::ip::basic_endpoint<asio::ip::tcp>;
  using resolver = asio::ip::basic_resolver<asio::ip::tcp>;

#if defined(ASIO_RDMA_BACKEND_ND)
  using queue_pair  = nd_queue_pair;
  using connector = nd_connector<tcp>;
  using listener  = nd_listener<tcp>;
#elif defined(ASIO_RDMA_BACKEND_VERBS)
  using queue_pair = ibv_queue_pair;
  using connector  = ibv_connector<tcp>;
  using listener   = ibv_listener<tcp>;

  // rdma_cm port space for create_id (control plane uses sockaddr like TCP).
  static rdma_port_space rdma_type() noexcept { return RDMA_PS_TCP; }
#endif

private:
  asio::ip::tcp impl_;
  explicit tcp(asio::ip::tcp impl) noexcept : impl_(impl) {}

public:
  static tcp v4() noexcept { return tcp{asio::ip::tcp::v4()}; }
  static tcp v6() noexcept { return tcp{asio::ip::tcp::v6()}; }

#if defined(ASIO_RDMA_BACKEND_ND)
  auto get_adapters(detail::nd_provider_t const& provider) const noexcept
      -> std::vector<detail::nd_adapter_ptr> {
    auto const family = impl_.family();
    if (family == AF_INET) {
      return provider.v4_adapters_;
    }
    return provider.v6_adapters_;
  }
#endif
};

}
