// mico-test: mico's self-tests and benchmark, kept out of the mico that ships.
// Built beside it, and run by `ctest`:
//   mico-test --selftest | --api-test | --bench
#include <cstdio>
#include <cstring>

namespace mico {
int run_selftest();
int run_api_test();
int run_bench();
}  // namespace mico

int main(int argc, char** argv) {
  if (argc == 2 && !strcmp(argv[1], "--selftest")) return mico::run_selftest();
  if (argc == 2 && !strcmp(argv[1], "--api-test")) return mico::run_api_test();
  if (argc == 2 && !strcmp(argv[1], "--bench")) return mico::run_bench();
  fprintf(stderr, "usage: mico-test --selftest | --api-test | --bench\n");
  return 2;
}
