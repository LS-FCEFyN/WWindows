#include <wwindows/input/policy/pointer_mapping.hpp>
#include "../support/test.hpp"
WW_TEST_CASE(buttons_map_to_linux_codes) {
  WW_TEST_CHECK(ww::input::policy::linux_button(1) == 0x110);
  WW_TEST_CHECK(ww::input::policy::linux_button(2) == 0x112);
  WW_TEST_CHECK(ww::input::policy::linux_button(3) == 0x111);
  WW_TEST_CHECK(ww::input::policy::linux_button(8) == 0);
}
WW_TEST_CASE(scroll_buttons_map_to_axes) {
  ww::input::policy::Axis a{};
  WW_TEST_CHECK(ww::input::policy::scroll_axis(4, a) && a.axis == 0 && a.value == -15.0);
  WW_TEST_CHECK(ww::input::policy::scroll_axis(5, a) && a.axis == 0 && a.value == 15.0);
  WW_TEST_CHECK(ww::input::policy::scroll_axis(6, a) && a.axis == 1 && a.value == -15.0);
  WW_TEST_CHECK(ww::input::policy::scroll_axis(7, a) && a.axis == 1 && a.value == 15.0);
}
WW_TEST_CASE(non_scroll_buttons_are_not_axes) {
  ww::input::policy::Axis a{};
  WW_TEST_CHECK(!ww::input::policy::scroll_axis(1, a));
}
