// Translation unit A -- includes the public headers (declarations only under
// ASIO_SEPARATE_COMPILATION) and odr-uses split symbols so the linker must
// resolve them from src_rdma.o (linkage check).
#include "rdma/rdma.hpp"
#include <string>
#include <system_error>
namespace rdma = asio::rdma;
std::string tu_a_msg() {
  return rdma::make_error_code(rdma::rdma_errc::invalid_device).message();
}
bool tu_a_memory_region_rejects_null_device() {
  try {
    rdma::rdma_memory_region mr{rdma::rdma_device_ptr{}, nullptr, 0};
    (void)mr;
  } catch (std::system_error const& e) {
    return e.code() ==
           rdma::make_error_code(rdma::rdma_errc::invalid_device);
  }
  return false;
}
bool tu_a_discover() {
  // exercises the backend discovery .ipp (get_devices / config compat / addresses)
  auto d = rdma::rdma_device_manager_t::instance().get_first_available_device({});
  return static_cast<bool>(d);
}
