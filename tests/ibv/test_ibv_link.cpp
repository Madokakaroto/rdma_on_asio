#include <iostream>

#include "rdma/rdma.hpp"

int main() {
  auto device = asio::rdma::rdma_device_manager_t::instance()
                    .get_first_available_device({});
  if (device) {
    auto address = asio::rdma::query_local_rdma_address(device);
    std::cout << "ibv device local address: " << address.to_string() << "\n";
  } else {
    std::cout << "no RDMA device available\n";
  }
  return 0;
}
