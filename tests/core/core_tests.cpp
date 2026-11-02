#include <wwindows/core/event_loop.hpp>
#include <wwindows/core/policy/surface_geometry.hpp>
#include <wwindows/core/policy/resize_policy.hpp>
#include <wwindows/core/detail/surface_index.hpp>
#include "../support/test.hpp"
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <thread>

namespace {
ssize_t read_retry_eintr(int fd, void *buffer, size_t size) {
  ssize_t result = -1;
  do {
    result = ::read(fd, buffer, size);
  } while (result < 0 && errno == EINTR);
  return result;
}
} // namespace

WW_TEST_CASE(clamp_size_preserves_valid_dimensions) {
  auto s = ww::core::policy::clamp_size(640, 480);
  WW_TEST_CHECK(s.width == 640 && s.height == 480);
}
WW_TEST_CASE(clamp_size_rejects_invalid_dimensions) {
  auto s = ww::core::policy::clamp_size(0, 999999);
  WW_TEST_CHECK(s.width == 1 && s.height == 65535);
}
WW_TEST_CASE(clamp_size_saturates_instead_of_wrapping) {
  // The bug this policy exists to prevent: a bare narrowing cast turns a
  // width of 65536 into 0, and X11 rejects a zero-sized window.
  auto s = ww::core::policy::clamp_size(65536, 70000);
  WW_TEST_CHECK_EQ(s.width, uint16_t{65535});
  WW_TEST_CHECK_EQ(s.height, uint16_t{65535});
}
WW_TEST_CASE(clamp_size_rejects_negative_dimensions) {
  auto s = ww::core::policy::clamp_size(-4, -1);
  WW_TEST_CHECK_EQ(s.width, uint16_t{1});
  WW_TEST_CHECK_EQ(s.height, uint16_t{1});
}
WW_TEST_CASE(event_loop_dispatches_and_can_stop_from_callback) {
  auto result = ww::EventLoop::create();
  WW_TEST_CHECK(result.has_value());
  auto& loop = **result;
  int fds[2]{};
  WW_TEST_CHECK(::pipe(fds) == 0);
  bool called = false;
  ssize_t bytes_read = -1;
  char consumed{};
  WW_TEST_CHECK(loop.add(fds[0], ww::IoIn, [&](uint32_t) {
    bytes_read = read_retry_eintr(fds[0], &consumed, 1);
    called = true;
    loop.stop(7);
  }).has_value());
  char c = 1;
  WW_TEST_CHECK(::write(fds[1], &c, 1) == 1);
  WW_TEST_CHECK(loop.run() == 7);
  WW_TEST_CHECK(called);
  WW_TEST_CHECK_EQ(bytes_read, ssize_t{1});
  WW_TEST_CHECK_EQ(consumed, char{1});
  loop.remove(fds[0]);
  ::close(fds[0]); ::close(fds[1]);
}

WW_TEST_CASE(event_loop_callback_may_remove_its_own_fd) {
  // A peer that hangs up is the natural moment to stop watching its fd, and
  // that removal happens from inside the very callback being run. The
  // capture is deliberately larger than std::function's small buffer, so the
  // callable lives on the heap and is freed by that removal: touching it
  // afterwards is a use-after-free that AddressSanitizer reports, not
  // something that merely happens to work.
  auto result = ww::EventLoop::create();
  WW_TEST_CHECK(result.has_value());
  auto &loop = **result;
  int fds[2]{};
  WW_TEST_CHECK(::pipe(fds) == 0);

  std::array<char, 256> ballast{};
  ballast.fill('x');
  int seen = 0;
  ssize_t bytes_read = -1;
  char consumed{};
  WW_TEST_CHECK(loop.add(fds[0], ww::IoIn, [&, ballast](uint32_t) {
    bytes_read = read_retry_eintr(fds[0], &consumed, 1);
    (void)loop.remove(fds[0]);
    seen = ballast[100] == 'x' ? 1 : -1; // read after the removal
    loop.stop(3);
  }).has_value());
  char c = 1;
  WW_TEST_CHECK(::write(fds[1], &c, 1) == 1);
  WW_TEST_CHECK(loop.run() == 3);
  WW_TEST_CHECK_EQ(seen, 1);
  WW_TEST_CHECK_EQ(bytes_read, ssize_t{1});
  WW_TEST_CHECK_EQ(consumed, char{1});
  ::close(fds[0]); ::close(fds[1]);
}

WW_TEST_CASE(event_loop_add_replaces_an_existing_callback) {
  // event_loop.hpp documents add() as "Replaces any existing entry", so the
  // callback registered last must be the one that runs.
  auto result = ww::EventLoop::create();
  WW_TEST_CHECK(result.has_value());
  auto &loop = **result;
  int fds[2]{};
  WW_TEST_CHECK(::pipe(fds) == 0);
  int which = 0;
  char sink{};
  WW_TEST_CHECK(loop.add(fds[0], ww::IoIn, [&](uint32_t) {
    which = 1;
    (void)read_retry_eintr(fds[0], &sink, 1);
    loop.stop(1);
  }).has_value());
  WW_TEST_CHECK(loop.add(fds[0], ww::IoIn, [&](uint32_t) {
    which = 2;
    (void)read_retry_eintr(fds[0], &sink, 1);
    loop.stop(2);
  }).has_value());
  char c = 1;
  WW_TEST_CHECK(::write(fds[1], &c, 1) == 1);
  (void)loop.run();
  WW_TEST_CHECK_EQ(which, 2);
  loop.remove(fds[0]);
  ::close(fds[0]); ::close(fds[1]);
}

WW_TEST_CASE(event_loop_dispatches_due_timers) {
  auto result = ww::EventLoop::create();
  WW_TEST_CHECK(result.has_value());
  auto &loop = **result;
  bool called = false;
  WW_TEST_CHECK(loop.schedule_at(loop.now(), [&] {
    called = true;
    loop.stop(6);
  }) != 0);
  WW_TEST_CHECK_EQ(loop.run(), 6);
  WW_TEST_CHECK(called);
}

WW_TEST_CASE(surface_index_window_binding_round_trip) {
  ww::SurfaceIndex index;
  auto *surface = reinterpret_cast<wl_resource *>(0x1234);
  index.bind_window(42, surface);
  auto it = index.find_window(42);
  WW_TEST_CHECK(it != index.window_end());
  WW_TEST_CHECK(it->second == surface);
  index.unbind_window(42);
  WW_TEST_CHECK(index.find_window(42) == index.window_end());
}
WW_TEST_CASE(surface_index_popup_stack_is_lifo) {
  ww::SurfaceIndex index;
  auto *first = reinterpret_cast<wl_resource *>(0x1);
  auto *second = reinterpret_cast<wl_resource *>(0x2);
  auto *third = reinterpret_cast<wl_resource *>(0x3);
  index.push_popup(first); index.push_popup(second); index.push_popup(third);
  WW_TEST_CHECK(index.popup_count() == 3);
  WW_TEST_CHECK(index.top_popup() == third);
  index.remove_popup(second);
  WW_TEST_CHECK(index.popup_count() == 2);
  index.discard_top_popup();
  WW_TEST_CHECK(index.top_popup() == first);
  index.discard_top_popup();
  WW_TEST_CHECK(index.popups_empty());
}

WW_TEST_CASE(surface_index_rebinding_replaces_stale_window_owner) {
  ww::SurfaceIndex index;
  auto *first = reinterpret_cast<wl_resource *>(0x100);
  auto *second = reinterpret_cast<wl_resource *>(0x200);

  index.bind_window(77, first);
  index.bind_window(77, second);
  auto it = index.find_window(77);
  WW_TEST_CHECK(it != index.window_end());
  WW_TEST_CHECK(it->second == second);
}

// --- resize policy ----------------------------------------------------------
namespace rp = ww::core::policy;

WW_TEST_CASE(configure_needed_when_nothing_was_ever_configured) {
  WW_TEST_CHECK(rp::configure_needed(std::nullopt, {800, 600}, {}));
}
WW_TEST_CASE(configure_not_needed_for_identical_size_and_states) {
  rp::ConfiguredState last{{800, 600}, {1, 2}};
  WW_TEST_CHECK(!rp::configure_needed(last, {800, 600}, {1, 2}));
}
WW_TEST_CASE(configure_needed_when_only_states_change) {
  // maximized -> tiled at an unchanged size must still reach the client.
  rp::ConfiguredState last{{800, 600}, {1}};
  WW_TEST_CHECK(rp::configure_needed(last, {800, 600}, {1, 2}));
}
WW_TEST_CASE(configure_needed_when_only_size_changes) {
  rp::ConfiguredState last{{800, 600}, {}};
  WW_TEST_CHECK(rp::configure_needed(last, {801, 600}, {}));
}

namespace {
rp::CommitInputs commit(rp::WindowSize committed, std::optional<rp::WindowSize> x11,
                        rp::WindowSize target) {
  rp::CommitInputs in;
  in.committed = committed;
  in.x11 = x11;
  in.target = target;
  return in;
}
} // namespace

WW_TEST_CASE(commit_matching_actual_x11_wins_over_target_match) {
  auto in = commit({640, 480}, rp::WindowSize{640, 480}, {640, 480});
  WW_TEST_CHECK(rp::classify_commit(in) == rp::CommitVerdict::MatchesX11);
}
WW_TEST_CASE(commit_matching_wm_target_while_x11_differs) {
  auto in = commit({640, 480}, rp::WindowSize{500, 400}, {640, 480});
  WW_TEST_CHECK(rp::classify_commit(in) == rp::CommitVerdict::MatchesWmTarget);
}
WW_TEST_CASE(first_frame_adopts_only_when_floating_and_not_waiting) {
  auto in = commit({640, 480}, std::nullopt, {800, 600});
  WW_TEST_CHECK(rp::classify_commit(in) == rp::CommitVerdict::FirstFrame);
  in.wm_owns_geometry = true;
  WW_TEST_CHECK(rp::classify_commit(in) == rp::CommitVerdict::Stale);
  in.wm_owns_geometry = false;
  in.was_waiting_for_frame = true;
  WW_TEST_CHECK(rp::classify_commit(in) == rp::CommitVerdict::Stale);
}
WW_TEST_CASE(later_mismatching_commit_is_stale) {
  auto in = commit({640, 480}, rp::WindowSize{800, 600}, {800, 600});
  in.had_previous_commit = true;
  WW_TEST_CHECK(rp::classify_commit(in) == rp::CommitVerdict::Stale);
}

// --- re-asserting the configure ---------------------------------------------
namespace {
rp::ReassertInputs reassert(rp::WindowSize committed, rp::WindowSize target) {
  rp::ReassertInputs in;
  in.committed = committed;
  in.target = target;
  in.size_is_enforced = true;
  in.all_configures_acked = true;
  return in;
}
} // namespace

WW_TEST_CASE(reassert_when_an_enforced_size_is_disobeyed) {
  WW_TEST_CHECK(rp::should_reassert_configure(reassert({512, 384}, {1024, 768})));
}
WW_TEST_CASE(never_reassert_when_the_size_is_not_enforced) {
  // Floating: the client's size is adopted instead.
  auto in = reassert({512, 384}, {1024, 768});
  in.size_is_enforced = false;
  WW_TEST_CHECK(!rp::should_reassert_configure(in));
}
WW_TEST_CASE(never_reassert_when_the_client_agrees) {
  WW_TEST_CHECK(!rp::should_reassert_configure(reassert({1024, 768}, {1024, 768})));
}
WW_TEST_CASE(never_reassert_before_the_last_configure_was_acknowledged) {
  auto in = reassert({512, 384}, {1024, 768});
  in.all_configures_acked = false;
  WW_TEST_CHECK(!rp::should_reassert_configure(in));
}
WW_TEST_CASE(reassert_once_per_distinct_committed_size) {
  auto in = reassert({512, 384}, {1024, 768});
  in.already_reasserted_against = rp::WindowSize{512, 384};
  WW_TEST_CHECK(!rp::should_reassert_configure(in)); // same wrong size: no loop
  in.committed = {256, 192};
  WW_TEST_CHECK(rp::should_reassert_configure(in)); // next zoom step
}
WW_TEST_CASE(never_reassert_against_an_unknown_or_empty_size) {
  WW_TEST_CHECK(!rp::should_reassert_configure(reassert({512, 384}, {0, 0})));
  WW_TEST_CHECK(!rp::should_reassert_configure(reassert({0, 0}, {1024, 768})));
}
