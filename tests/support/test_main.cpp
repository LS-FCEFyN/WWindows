#include "test.hpp"

#include <wwindows/support/log.hpp>

#include <cstdio>
#include <cstdlib>

namespace ww_test {

int run(std::string_view suite) {
  int failed = 0;
  for (const auto &test : registry()) {
    try {
      test.fn();
      std::printf("[PASS] %.*s/%.*s\n", static_cast<int>(suite.size()),
                  suite.data(), static_cast<int>(test.name.size()),
                  test.name.data());
    } catch (const std::exception &e) {
      ++failed;
      std::fprintf(stderr, "[FAIL] %.*s/%.*s: %s\n",
                   static_cast<int>(suite.size()), suite.data(),
                   static_cast<int>(test.name.size()), test.name.data(),
                   e.what());
    }
  }
  std::printf("%zu test(s), %d failure(s)\n", registry().size(), failed);
  // An empty suite means the tests failed to register -- that is a failure,
  // not a pass. A silently empty suite is the exact failure mode this
  // framework exists to prevent.
  if (registry().empty()) {
    std::fprintf(stderr, "[FAIL] %.*s: suite registered no tests\n",
                 static_cast<int>(suite.size()), suite.data());
    return 1;
  }
  return failed == 0 ? 0 : 1;
}

} // namespace ww_test

#ifndef WW_TEST_SUITE
#define WW_TEST_SUITE "unnamed"
#endif

int main() {
  // Production logging would bury the test results. Developers can still get
  // it back by setting WWINDOWS_LOG_LEVEL explicitly, which is exactly what
  // you want when a test fails and you need the subsystem trace.
  if (!std::getenv("WWINDOWS_LOG_LEVEL"))
    ww::log::set_level(ww::log::Level::Error);
  return ww_test::run(WW_TEST_SUITE);
}
