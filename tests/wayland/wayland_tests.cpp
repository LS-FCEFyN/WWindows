#include <wwindows/adapters/wayland/policy/scale.hpp>
#include "../support/test.hpp"
WW_TEST_CASE(scale_defaults_for_missing_physical_size) {
  WW_TEST_CHECK(ww::wayland::policy::compute_scale_120ths(1920, 0) == 120);
  WW_TEST_CHECK(ww::wayland::policy::compute_scale_120ths(0, 300) == 120);
}
WW_TEST_CASE(scale_is_one_at_96_dpi) {
  // 1920 px / (508 mm / 25.4) ~= 96 dpi.
  WW_TEST_CHECK(ww::wayland::policy::compute_scale_120ths(1920, 508) == 120);
}
WW_TEST_CASE(scale_is_clamped_to_protocol_policy_limits) {
  WW_TEST_CHECK(ww::wayland::policy::compute_scale_120ths(100000, 100) == 480);
}
