#include <iostream>
#include <string>
std::string tu_a_msg();
bool tu_a_memory_region_rejects_null_device();
bool tu_a_discover();
std::string tu_b_msg();
int main() {
  std::cout << tu_a_msg() << " | " << tu_b_msg() << "\n";
  if (!tu_a_memory_region_rejects_null_device()) {
    std::cerr << "[FAIL] rdma_memory_region did not reject a null device\n";
    return 1;
  }
  try {
    (void)tu_a_discover();  // no RDMA device on this host -> nullptr/throw is fine
  } catch (...) {
  }
  std::cout << "[PASS] rdma_on_asio separate-compilation smoke (ODR + linkage)\n";
  return 0;
}
