#include <wwindows/support/error.hpp>
#include <wwindows/support/unique_fd.hpp>
#include "test.hpp"
#include <unistd.h>

WW_TEST_CASE(error_factory_preserves_code_and_message) {
  const auto e = ww::Error::x11_connect("cannot connect");
  WW_TEST_CHECK(e.code == ww::ErrorCode::X11ConnectFailed);
  WW_TEST_CHECK(e.message == "cannot connect");
}
WW_TEST_CASE(unique_fd_is_move_only_owner) {
  int fds[2]{};
  WW_TEST_CHECK(::pipe(fds) == 0);
  ww::UniqueFd a{fds[0]};
  ww::UniqueFd b{std::move(a)};
  WW_TEST_CHECK(!a.valid());
  WW_TEST_CHECK(b.valid());
  b.reset();
  ::close(fds[1]);
}
