// Translation unit B -- second TU including the same headers. If any non-template
// implementation were left defined (non-inline) in a header, linking A+B+src would
// fail with "multiple definition" (ODR check).
#include "rdma/rdma.hpp"
namespace rdma = asio::rdma;
std::string tu_b_msg() {
  return rdma::make_error_code(rdma::rdma_errc::address_family_not_supported).message();
}
