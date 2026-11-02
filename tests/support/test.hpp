#pragma once

// Minimal self-registering test framework.
//
// A test declares itself with WW_TEST_CASE and is *automatically* added to
// the suite. The previous version required every test to be named twice --
// once at the definition and again in a hand-written list inside main() --
// which meant a test could be written, compiled, and silently never run.
// Registration is now a side effect of the definition, so that cannot happen.
//
// main() lives in test_main.cpp and is linked into every suite binary; the
// suite name comes from the WW_TEST_SUITE compile definition set in
// tests/CMakeLists.txt.

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace ww_test {

struct Failure : std::exception {
  std::string message;
  explicit Failure(std::string m) : message(std::move(m)) {}
  const char *what() const noexcept override { return message.c_str(); }
};

struct TestCase {
  std::string_view name;
  void (*fn)();
};

// Function-local static: safe to reference from the dynamic initializers of
// the Registrar objects below regardless of translation-unit ordering.
inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}

struct Registrar {
  Registrar(std::string_view name, void (*fn)()) {
    registry().push_back({name, fn});
  }
};

inline void check(bool condition, std::string_view expression,
                  std::string_view file, int line) {
  if (!condition)
    throw Failure(std::string(file) + ":" + std::to_string(line) + ": CHECK(" +
                  std::string(expression) + ") failed");
}

// Equality check that reports both operands on failure. A bare CHECK(a == b)
// only tells you that two values differed, not what they were.
template <typename A, typename B>
void check_eq(const A &actual, const B &expected, std::string_view expression,
              std::string_view file, int line) {
  if (!(actual == expected)) {
    std::string msg = std::string(file) + ":" + std::to_string(line) +
                      ": CHECK_EQ(" + std::string(expression) + ") failed";
    if constexpr (requires { std::to_string(actual); std::to_string(expected); })
      msg += ": got " + std::to_string(actual) + ", expected " +
             std::to_string(expected);
    throw Failure(msg);
  }
}

int run(std::string_view suite);

} // namespace ww_test

#define WW_TEST_CHECK(x) ::ww_test::check((x), #x, __FILE__, __LINE__)
#define WW_TEST_CHECK_EQ(a, b)                                                 \
  ::ww_test::check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)

// Defining a test registers it. The forward declaration lets the Registrar
// take the address before the body is seen.
#define WW_TEST_CASE(name)                                                     \
  static void name();                                                          \
  static const ::ww_test::Registrar ww_test_registrar_##name{#name, name};     \
  static void name()
