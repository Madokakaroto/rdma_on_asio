// Generated-style thin entry point for the asio_perftest family (Stage 9a).
// Heavy code lives in the asio_perftest_obj object library; this only selects
// the operation/metric and calls the shared CLI.
#include "asio_perftest_core.hpp"

int main(int argc, char** argv) {
  return asio_perftest::cli_main(argc, argv);
}
