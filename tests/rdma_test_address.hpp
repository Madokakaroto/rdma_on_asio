#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "asio/ip/address.hpp"
#include "rdma/rdma.hpp"

namespace rdma_test {

struct local_endpoint_args {
  asio::ip::address address;
  std::uint16_t port;

  std::string address_string() const {
    return address.to_string();
  }

  asio::rdma::tcp port_space() const noexcept {
    return address.is_v4() ? asio::rdma::tcp::v4() : asio::rdma::tcp::v6();
  }

  asio::rdma::tcp::endpoint endpoint() const {
    return asio::rdma::tcp::endpoint(address, port);
  }
};

inline asio::rdma::rdma_device_ptr first_available_device() {
  auto device =
      asio::rdma::rdma_device_manager_t::instance().get_first_available_device({});
  if (!device) {
    throw std::runtime_error("no RDMA device available");
  }
  return device;
}

inline asio::ip::address local_device_address(asio::rdma::tcp port_space) {
  auto device = first_available_device();
  return port_space.any_endpoint(0).address().is_v4()
             ? device->get_v4_address()
             : device->get_v6_address();
}

inline std::string local_device_address_string(asio::rdma::tcp port_space) {
  return local_device_address(port_space).to_string();
}

inline asio::rdma::tcp port_space_for(asio::ip::address const& address) {
  return address.is_v4() ? asio::rdma::tcp::v4() : asio::rdma::tcp::v6();
}

inline asio::rdma::tcp port_space_for(std::string_view address) {
  return port_space_for(asio::ip::make_address(std::string(address)));
}

inline asio::rdma::tcp::endpoint endpoint_for(asio::ip::address const& address,
                                              std::uint16_t port) {
  return asio::rdma::tcp::endpoint(address, port);
}

inline asio::rdma::tcp::endpoint endpoint_for(std::string_view address,
                                              std::uint16_t port) {
  return endpoint_for(asio::ip::make_address(std::string(address)), port);
}

inline bool is_port_text(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  std::uint32_t port = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    port = port * 10u + static_cast<std::uint32_t>(ch - '0');
    if (port > 65535u) {
      return false;
    }
  }
  return true;
}

inline std::uint16_t parse_port(std::string_view value) {
  if (!is_port_text(value)) {
    throw std::invalid_argument("expected TCP/RDMA port");
  }
  std::uint32_t port = 0;
  for (char ch : value) {
    port = port * 10u + static_cast<std::uint32_t>(ch - '0');
  }
  return static_cast<std::uint16_t>(port);
}

inline std::uint16_t parse_port_arg(int argc, char* argv[],
                                    std::uint16_t default_port) {
  if (argc <= 1) {
    return default_port;
  }
  if (argc > 2) {
    throw std::invalid_argument("usage: optional argument is [port]");
  }
  return parse_port(argv[1]);
}

inline local_endpoint_args query_local_endpoint_with_port_arg(
    int argc, char* argv[], std::uint16_t default_port) {
  return local_endpoint_args{
      local_device_address(asio::rdma::tcp::v4()),
      parse_port_arg(argc, argv, default_port),
  };
}

}  // namespace rdma_test
