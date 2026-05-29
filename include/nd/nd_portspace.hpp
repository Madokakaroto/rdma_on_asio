#pragma once

#include "asio/ip/tcp.hpp"
#include "nd/detail/nd_impl_types.hpp"

namespace asio::rdma::roce {
namespace v2 {

class tcp {
 public:
  // endpoint type
  using endpoint = asio::ip::basic_endpoint<asio::ip::tcp>;

  // resolver type
  using resolver = asio::ip::basic_resolver<asio::ip::tcp>;

private:
  asio::ip::tcp impl_;

private:
  explicit tcp(asio::ip::tcp impl) noexcept : impl_(impl) {
  }

public:
  static tcp v4() noexcept { 
    return tcp{asio::ip::tcp::v4()}; 
  }

  static tcp v6() noexcept { 
    return tcp{asio::ip::tcp::v6()}; 
  }

  auto get_adapters(detail::nd_provider_t const& provider) const noexcept
      -> std::vector<detail::nd_adapter_ptr> {
    auto const family = impl_.family();
    if (family == ASIO_OS_DEF(AF_INET)) {
      return provider.v4_adapters_;
    }
    return provider.v6_adapters_;
  }
};

}
}

