#include <iostream>
#include <string>
std::string tu_a_msg();
bool tu_a_discover();
std::string tu_b_msg();
int main() {
  std::cout << tu_a_msg() << " | " << tu_b_msg() << "\n";
  try {
    (void)tu_a_discover();  // no RDMA device on this host -> nullptr/throw is fine
  } catch (...) {
  }
  std::cout << "[PASS] rdma_on_asio separate-compilation smoke (ODR + linkage)\n";
  return 0;
}
