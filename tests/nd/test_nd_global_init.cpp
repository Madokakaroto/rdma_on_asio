#include <cassert>
#include <iostream>
#include <system_error>

#include <WinSock2.h>
#include <ndsupport.h>

#include "rdma/nd/detail/nd_asio_manual_init.hpp"

void test_wsa_not_initialized_before_global() {
  // manual_winsock_init suppresses asio's auto WSAStartup.
  // Before nd_global_t is constructed, WSA should NOT be usable.
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(s == INVALID_SOCKET);
  assert(::WSAGetLastError() == WSANOTINITIALISED);
  std::cout << "[PASS] socket fails with WSANOTINITIALISED before nd_global_t\n";
}

void test_wsa_usable_after_global() {
  asio::rdma::detail::nd_global_t global{};

  // After nd_global_t construction, WSA 2.2 should be ready.
  SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(s != INVALID_SOCKET);
  ::closesocket(s);
  std::cout << "[PASS] socket creation works after nd_global_t init\n";
}

void test_wsa_version_is_2_2() {
  asio::rdma::detail::nd_global_t global{};

  // WSAStartup is reference-counted; calling again to query version is safe.
  WSADATA wsaData;
  int ret = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
  assert(ret == 0);
  assert(LOBYTE(wsaData.wVersion) == 2);
  assert(HIBYTE(wsaData.wVersion) == 2);
  ::WSACleanup();
  std::cout << "[PASS] WSA version is 2.2\n";
}

int main() {
  try {
    test_wsa_not_initialized_before_global();
    test_wsa_usable_after_global();
    test_wsa_version_is_2_2();
    std::cout << "\nAll nd_global_init tests passed.\n";
    return 0;
  }
  catch (std::system_error const& e) {
    std::cerr << "nd_global_t init failed: " << e.what() << "\n";
    return 1;
  }
}
