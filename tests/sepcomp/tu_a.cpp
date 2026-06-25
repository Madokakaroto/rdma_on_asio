// Translation unit A -- includes the public headers (declarations only under
// ASIO_SEPARATE_COMPILATION) and odr-uses split symbols so the linker must
// resolve them from src_rdma.o (linkage check).
#include "rdma/rdma.hpp"
namespace rdma = asio::rdma;
std::string tu_a_msg() {
  return rdma::make_error_code(rdma::rdma_errc::invalid_device).message();
}
bool tu_a_discover() {
  // exercises the backend discovery .ipp (get_devices / config compat / addresses)
  auto d = rdma::rdma_device_manager_t::instance().get_first_available_device({});
  return static_cast<bool>(d);
}
