// Behavioral tests for the core surface state machine.
//
// These drive the real SurfaceManager against a real wl_display and a
// recording fake backend. They are the tests the ports-and-adapters split
// was supposed to make possible: no X server, no GPU, no Wayland client
// process, but the production code path is genuinely executed.
//
// What is asserted here is the *contract between core and the backend port*
// -- which backend calls a given protocol interaction must produce, and in
// what order. That is the contract a second backend implementation would
// have to satisfy, so these double as the port's conformance spec.

#include <algorithm>
#include "../fakes/fake_window_backend.hpp"
#include "../fakes/fake_resize_timer.hpp"
#include <wwindows/support/frame_diag.hpp>
#include "../fakes/wayland_fixture.hpp"
#include "../support/test.hpp"

#include <wwindows/core/surface_manager.hpp>

using ww_test::FakeResizeTimer;
using ww_test::FakeWindowBackend;
using ww_test::WaylandFixture;

namespace {

// Owns the three collaborators in the same relative order as the production
// composition root (src/application.cpp).
//
// This ordering is load-bearing, not cosmetic. ~SurfaceManager does not
// unregister the wl_resource destroy listeners that set_toplevel and
// attach_buffer install, so the wl_display must be torn down *first* --
// while the Surface those listeners write into is still alive.
// Declaring the fixture after the manager makes the display die first,
// because members are destroyed in reverse declaration order. Get this
// backwards and the listeners fire into freed memory; the ordering test at
// the bottom of this file pins the requirement down.
struct Harness {
  FakeWindowBackend backend;
  FakeResizeTimer timer;
  ww::SurfaceManager sm{backend, &timer}; // destroyed second
  WaylandFixture wl;                     // destroyed first
};

// Promote a fresh wl_surface to a mapped toplevel; returns the surface.
wl_resource *make_toplevel(ww::SurfaceManager &sm, WaylandFixture &wl) {
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  sm.set_toplevel(surface, wl.xdg_toplevel(), wl.xdg_surface());
  return surface;
}

} // namespace

// --- toplevel lifecycle -----------------------------------------------------

WW_TEST_CASE(surface_without_a_role_creates_no_window) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  sm.create_surface(wl.surface());

  // A bare wl_surface has no role yet, so there is nothing to show.
  WW_TEST_CHECK(!backend.called("create_window"));
}

WW_TEST_CASE(set_toplevel_creates_exactly_one_window_but_does_not_map_it) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  make_toplevel(sm, wl);

  WW_TEST_CHECK_EQ(backend.count("create_window"), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.windows.size(), std::size_t{1});
  // Mapping waits for the first commit, so that title / app_id / parent /
  // size limits are on the window by the time a window manager first sees
  // it.
  WW_TEST_CHECK_EQ(backend.count("map_window"), std::size_t{0});
  WW_TEST_CHECK(!backend.windows.begin()->second.mapped);
}

WW_TEST_CASE(first_commit_maps_the_toplevel_exactly_once) {
  Harness h;
  auto &backend = h.backend;

  wl_resource *surface = make_toplevel(h.sm, h.wl);
  h.sm.commit(surface);

  WW_TEST_CHECK_EQ(backend.count("map_window"), std::size_t{1});
  WW_TEST_CHECK(backend.windows.begin()->second.mapped);

  // Later commits are ordinary frames, not new maps.
  h.sm.commit(surface);
  h.sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.count("map_window"), std::size_t{1});
}

WW_TEST_CASE(a_toplevel_is_fully_described_before_it_is_mapped) {
  Harness h;
  auto &backend = h.backend;

  wl_resource *surface = make_toplevel(h.sm, h.wl);
  h.sm.set_title(surface, "a title");
  h.sm.set_app_id(surface, "foot");
  h.sm.set_min_size(surface, 200, 100);
  h.sm.set_max_size(surface, 800, 0);
  h.sm.commit(surface);

  // The order is the point: a window manager decides workspace and
  // floating rules the moment it sees the map, so everything it might
  // match on has to be set first.
  const auto index_of = [&](const char *call) {
    const auto it = std::find(backend.calls.begin(), backend.calls.end(), call);
    return it == backend.calls.end() ? -1
                                     : static_cast<int>(it - backend.calls.begin());
  };
  const int map = index_of("map_window");
  WW_TEST_CHECK(map >= 0);
  WW_TEST_CHECK(index_of("set_window_title") >= 0 &&
                index_of("set_window_title") < map);
  WW_TEST_CHECK(index_of("set_window_class") >= 0 &&
                index_of("set_window_class") < map);
  WW_TEST_CHECK(index_of("set_window_client_process") >= 0 &&
                index_of("set_window_client_process") < map);
  WW_TEST_CHECK(index_of("set_window_size_limits") >= 0 &&
                index_of("set_window_size_limits") < map);
}

WW_TEST_CASE(a_surface_that_never_commits_is_never_mapped) {
  Harness h;
  wl_resource *surface = make_toplevel(h.sm, h.wl);
  h.sm.set_title(surface, "still setting up");
  h.sm.set_app_id(surface, "foot");

  WW_TEST_CHECK(!h.backend.called("map_window"));
}

WW_TEST_CASE(set_app_id_becomes_both_halves_of_wm_class) {
  Harness h;
  auto &backend = h.backend;

  wl_resource *surface = make_toplevel(h.sm, h.wl);
  h.sm.set_app_id(surface, "org.example.Editor");

  const auto *w = backend.window(backend.windows.begin()->first);
  WW_TEST_CHECK(w != nullptr);
  if (!w)
    return;
  WW_TEST_CHECK(w->wm_class_instance == "org.example.Editor");
  WW_TEST_CHECK(w->wm_class == "org.example.Editor");
}

WW_TEST_CASE(set_app_id_can_change_after_the_window_is_mapped) {
  Harness h;
  auto &backend = h.backend;

  wl_resource *surface = make_toplevel(h.sm, h.wl);
  h.sm.set_app_id(surface, "first");
  h.sm.commit(surface);
  h.sm.set_app_id(surface, "second");

  const auto *w = backend.window(backend.windows.begin()->first);
  WW_TEST_CHECK(w != nullptr);
  if (w)
    WW_TEST_CHECK(w->wm_class == "second");
}

WW_TEST_CASE(size_limits_are_double_buffered_until_commit) {
  Harness h;
  auto &backend = h.backend;

  wl_resource *surface = make_toplevel(h.sm, h.wl);
  const uint32_t win = backend.windows.begin()->first;
  h.sm.set_min_size(surface, 320, 200);
  h.sm.set_max_size(surface, 1024, 768);

  // Nothing has reached the window yet ...
  const auto *w = backend.window(win);
  WW_TEST_CHECK(w != nullptr);
  if (w)
    WW_TEST_CHECK(!w->size_limits.any());

  h.sm.commit(surface);
  const auto &lim = backend.windows.at(win).size_limits;
  WW_TEST_CHECK_EQ(lim.min_width, 320);
  WW_TEST_CHECK_EQ(lim.min_height, 200);
  WW_TEST_CHECK_EQ(lim.max_width, 1024);
  WW_TEST_CHECK_EQ(lim.max_height, 768);

  // ... and an unchanged commit does not resend them.
  const auto sent = backend.count("set_window_size_limits");
  h.sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.count("set_window_size_limits"), sent);
}

WW_TEST_CASE(setting_one_size_bound_in_a_later_commit_keeps_the_other) {
  // The pending limits are seeded from the committed ones, so a client that
  // sets min_size once and max_size later must not lose the first.
  Harness h;
  auto &backend = h.backend;

  wl_resource *surface = make_toplevel(h.sm, h.wl);
  const uint32_t win = backend.windows.begin()->first;
  h.sm.set_min_size(surface, 320, 200);
  h.sm.commit(surface);

  h.sm.set_max_size(surface, 1024, 768);
  h.sm.commit(surface);

  const auto &lim = backend.windows.at(win).size_limits;
  WW_TEST_CHECK_EQ(lim.min_width, 320);
  WW_TEST_CHECK_EQ(lim.min_height, 200);
  WW_TEST_CHECK_EQ(lim.max_width, 1024);
  WW_TEST_CHECK_EQ(lim.max_height, 768);
}

WW_TEST_CASE(negative_size_limits_are_read_as_no_limit) {
  Harness h;
  auto &backend = h.backend;

  wl_resource *surface = make_toplevel(h.sm, h.wl);
  h.sm.set_min_size(surface, -5, -1);
  h.sm.set_max_size(surface, 640, -1);
  h.sm.commit(surface);

  const auto &lim =
      backend.windows.at(backend.windows.begin()->first).size_limits;
  WW_TEST_CHECK_EQ(lim.min_width, 0);
  WW_TEST_CHECK_EQ(lim.min_height, 0);
  WW_TEST_CHECK_EQ(lim.max_width, 640);
  WW_TEST_CHECK_EQ(lim.max_height, 0);
}

WW_TEST_CASE(set_parent_becomes_transient_for_the_parents_window) {
  Harness h;
  auto &backend = h.backend;

  wl_resource *parent = make_toplevel(h.sm, h.wl);
  const uint32_t parent_win = backend.windows.begin()->first;
  wl_resource *child = make_toplevel(h.sm, h.wl);
  uint32_t child_win = 0;
  for (const auto &[id, w] : backend.windows)
    if (id != parent_win)
      child_win = id;
  WW_TEST_CHECK(child_win != 0);

  h.sm.set_toplevel_parent(child, parent);
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).transient_for, parent_win);

  // A null parent makes it independent again.
  h.sm.set_toplevel_parent(child, nullptr);
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).transient_for, uint32_t{0});
}

WW_TEST_CASE(a_toplevel_cannot_be_its_own_parent) {
  Harness h;
  auto &backend = h.backend;

  wl_resource *surface = make_toplevel(h.sm, h.wl);
  h.sm.set_toplevel_parent(surface, surface);

  WW_TEST_CHECK_EQ(
      backend.windows.at(backend.windows.begin()->first).transient_for,
                   uint32_t{0});
}

WW_TEST_CASE(the_window_is_tagged_with_the_clients_pid) {
  Harness h;
  auto &backend = h.backend;

  make_toplevel(h.sm, h.wl);

  // The fixture's client lives in this process, on the far end of a
  // socketpair, so SO_PEERCRED reports our own pid.
  WW_TEST_CHECK_EQ(
      backend.windows.at(backend.windows.begin()->first).client_pid,
                   static_cast<uint32_t>(::getpid()));
}

WW_TEST_CASE(set_toplevel_twice_reuses_the_same_window) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  sm.set_toplevel(surface, wl.xdg_toplevel(), wl.xdg_surface());
  sm.set_toplevel(surface, wl.xdg_toplevel(), wl.xdg_surface());

  // Re-issuing the role must not leak a second X11 window.
  WW_TEST_CHECK_EQ(backend.count("create_window"), std::size_t{1});
}

WW_TEST_CASE(toplevel_window_is_reachable_by_window_id) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t win = backend.windows.begin()->first;

  // This is the lookup the input layer depends on through
  // ISurfaceInteractions::surface_for_window.
  WW_TEST_CHECK(sm.surface_for_window(win) == surface);
  WW_TEST_CHECK(sm.surface_for_window(win + 1) == nullptr);
}

WW_TEST_CASE(surface_local_origin_reports_the_declared_window_geometry_origin) {
  Harness h;
  wl_resource *surface = make_toplevel(h.sm, h.wl);

  int32_t x = -1, y = -1;
  h.sm.surface_local_origin(surface, x, y);
  // No geometry declared yet: the whole buffer is the window.
  WW_TEST_CHECK_EQ(x, int32_t{0});
  WW_TEST_CHECK_EQ(y, int32_t{0});

  // Double-buffered against commit (see PendingState::window_geometry) --
  // set_window_geometry alone must not move it yet.
  h.sm.set_window_geometry(surface, 26, 22, 1024, 731);
  h.sm.surface_local_origin(surface, x, y);
  WW_TEST_CHECK_EQ(x, int32_t{0});
  WW_TEST_CHECK_EQ(y, int32_t{0});

  h.sm.commit(surface);
  h.sm.surface_local_origin(surface, x, y);
  WW_TEST_CHECK_EQ(x, int32_t{26});
  WW_TEST_CHECK_EQ(y, int32_t{22});

  // A zero-sized geometry clears it again, also only once committed.
  h.sm.set_window_geometry(surface, 26, 22, 0, 0);
  h.sm.commit(surface);
  h.sm.surface_local_origin(surface, x, y);
  WW_TEST_CHECK_EQ(x, int32_t{0});
  WW_TEST_CHECK_EQ(y, int32_t{0});

  // An unknown surface has no origin either.
  x = y = -1;
  h.sm.surface_local_origin(reinterpret_cast<wl_resource *>(0x1), x, y);
  WW_TEST_CHECK_EQ(x, int32_t{0});
  WW_TEST_CHECK_EQ(y, int32_t{0});
}

WW_TEST_CASE(destroying_a_surface_destroys_its_window_and_clears_the_lookup) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t win = backend.windows.begin()->first;

  sm.destroy_surface(surface);

  WW_TEST_CHECK(backend.destroyed.count(win) == 1);
  WW_TEST_CHECK(sm.surface_for_window(win) == nullptr);
}

WW_TEST_CASE(set_title_reaches_the_backend) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_title(surface, "hello");

  const uint32_t win = backend.windows.begin()->first;
  const auto *created_window = backend.window(win);
  WW_TEST_CHECK(created_window != nullptr);
  if (created_window)
    WW_TEST_CHECK(created_window->title == "hello");
}

WW_TEST_CASE(set_title_on_a_roleless_surface_is_ignored) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  sm.set_title(surface, "hello");

  // No window exists yet, so there is nothing to title -- and crucially this
  // must not crash or fabricate a window.
  WW_TEST_CHECK(!backend.called("set_window_title"));
}

WW_TEST_CASE(unknown_surfaces_are_ignored_rather_than_crashing) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  // A client can destroy objects in an order the compositor did not expect.
  // Every one of these takes a surface the manager has never seen.
  wl_resource *stranger = wl.surface();
  sm.destroy_surface(stranger);
  sm.set_title(stranger, "x");
  sm.set_toplevel(stranger, wl.xdg_toplevel(), wl.xdg_surface());
  sm.destroy_popup(stranger);
  sm.destroy_subsurface_role(stranger);
  sm.set_subsurface_position(stranger, 1, 2);

  WW_TEST_CHECK(backend.windows.empty());
}

// --- subsurfaces ------------------------------------------------------------

WW_TEST_CASE(subsurface_of_a_mapped_parent_becomes_a_child_window) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  const uint32_t parent_win = backend.windows.begin()->first;

  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);

  WW_TEST_CHECK_EQ(backend.count("create_child_window"), std::size_t{1});
  const uint32_t child_win = sm.surface_for_window(parent_win) == parent
                                 ? backend.next_id - 1
                                 : 0;
  WW_TEST_CHECK(backend.window(child_win) != nullptr);
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).parent, parent_win);
}

WW_TEST_CASE(a_cursor_surface_cannot_also_become_a_subsurface) {
  // One surface, one role: the cursor role must not be silently overwritten
  // by (or stacked with) a later get_subsurface.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *surf = wl.surface();
  sm.create_surface(surf);
  sm.set_cursor_surface(surf, 0, 0, {});
  sm.create_subsurface(wl.subsurface(), surf, parent);

  WW_TEST_CHECK_EQ(backend.count("create_child_window"), std::size_t{0});
}

WW_TEST_CASE(a_subsurface_cannot_also_become_a_cursor_surface) {
  // The gap case: a subsurface whose parent is not realized yet has no X11
  // window, so "already has a window" cannot be what rejects set_cursor. The
  // role itself has to.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = wl.surface();
  sm.create_surface(parent);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);

  bool cursor_changed = false;
  sm.set_cursor_surface(child, 0, 0, [&] { cursor_changed = true; });
  WW_TEST_CHECK(!cursor_changed);

  // It must still be a subsurface: realizing the parent creates its window.
  sm.set_toplevel(parent, wl.xdg_toplevel(), wl.xdg_surface());
  sm.commit(child);
  sm.commit(parent);
  WW_TEST_CHECK_EQ(backend.count("create_child_window"), std::size_t{1});
}

WW_TEST_CASE(subsurface_created_before_its_parent_is_realized_lazily) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  // Parent exists as a wl_surface but has no role (so no X11 window) yet.
  wl_resource *parent = wl.surface();
  sm.create_surface(parent);

  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);

  // Nothing can be created yet -- there is no parent window to nest into.
  WW_TEST_CHECK(!backend.called("create_child_window"));

  // Once the parent is promoted, the child's own (synchronized, the
  // default) commit only caches -- it takes the parent's own next
  // applied state to actually realize and cascade into it.
  sm.set_toplevel(parent, wl.xdg_toplevel(), wl.xdg_surface());
  sm.commit(child);
  WW_TEST_CHECK(!backend.called("create_child_window"));
  sm.commit(parent);

  WW_TEST_CHECK_EQ(backend.count("create_child_window"), std::size_t{1});
}

WW_TEST_CASE(subsurface_position_moves_the_child_window) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);

  const uint32_t child_win = backend.next_id - 1;
  sm.set_subsurface_position(child, 30, 40);

  // Double-buffered against the *parent's* next applied state, not this
  // call -- see Subsurface::pending_position.
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).x, int16_t{0});
  sm.commit(parent);

  WW_TEST_CHECK_EQ(backend.windows.at(child_win).x, int16_t{30});
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).y, int16_t{40});
}

WW_TEST_CASE(destroying_a_parent_never_double_destroys_its_children) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  backend.clear_calls();
  sm.destroy_surface(parent);

  // X11 destroys child windows with their parent. Issuing a second explicit
  // destroy on an id the server already invalidated produces BadWindow, so
  // the child must be *forgotten*, never destroyed.
  WW_TEST_CHECK(backend.forgotten.count(child_win) == 1);
  WW_TEST_CHECK(backend.destroyed.count(child_win) == 0);
}

WW_TEST_CASE(destroying_the_subsurface_role_keeps_the_surface_alive) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  sm.destroy_subsurface_role(child);

  // wl_subsurface.destroy drops the role but leaves the wl_surface usable,
  // so the surface must still be re-rolable afterwards.
  WW_TEST_CHECK(backend.destroyed.count(child_win) == 1);
  backend.clear_calls();
  sm.set_toplevel(child, wl.xdg_toplevel(), wl.xdg_surface());
  WW_TEST_CHECK_EQ(backend.count("create_window"), std::size_t{1});
}

WW_TEST_CASE(a_surface_that_already_has_a_role_cannot_become_a_subsurface) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *other = make_toplevel(sm, wl);

  backend.clear_calls();
  sm.create_subsurface(wl.subsurface(), other, parent);

  WW_TEST_CHECK(!backend.called("create_child_window"));
}

// --- wl_subsurface sync/desync and double-buffering -------------------------

WW_TEST_CASE(a_synchronized_commit_caches_rather_than_applies) {
  // The default -- "Initially, a sub-surface is in the synchronized
  // mode" (xdg-shell). A lone commit on the child, with no parent commit
  // following it, must not touch the window at all.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  sm.attach_buffer(child, wl.dmabuf_buffer(320, 240));
  sm.commit(child);

  WW_TEST_CHECK_EQ(backend.window(child_win)->width, uint16_t{1});

  // The parent's own apply is what actually cascades it in.
  sm.commit(parent);
  WW_TEST_CHECK_EQ(backend.window(child_win)->width, uint16_t{320});
  WW_TEST_CHECK_EQ(backend.window(child_win)->height, uint16_t{240});
}

WW_TEST_CASE(a_desynchronized_commit_applies_directly) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  sm.set_subsurface_sync(child, false);
  sm.attach_buffer(child, wl.dmabuf_buffer(320, 240));
  sm.commit(child);

  // No parent commit needed.
  WW_TEST_CHECK_EQ(backend.window(child_win)->width, uint16_t{320});
}

WW_TEST_CASE(a_desynchronized_child_of_a_synchronized_parent_still_caches) {
  // "Even if a sub-surface is set to desynchronized, a parent sub-surface
  // may override it to behave as synchronized." A grandchild explicitly
  // desynced still waits on the ultimate (toplevel) ancestor's apply if
  // its own parent -- itself a subsurface -- is still synchronized (the
  // default, left untouched here).
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *toplevel = make_toplevel(sm, wl);
  wl_resource *mid = wl.surface();
  sm.create_surface(mid);
  sm.create_subsurface(wl.subsurface(), mid, toplevel);
  sm.commit(mid); // cached; mid itself is never applied independently here

  wl_resource *grandchild = wl.surface();
  sm.create_surface(grandchild);
  sm.create_subsurface(wl.subsurface(), grandchild, mid);
  const uint32_t grandchild_win = backend.next_id - 1;

  sm.set_subsurface_sync(grandchild, false); // desynced itself...
  sm.attach_buffer(grandchild, wl.dmabuf_buffer(100, 50));
  sm.commit(grandchild);

  // ...but mid (its parent) is still synchronized, so this still caches.
  WW_TEST_CHECK_EQ(backend.window(grandchild_win)->width, uint16_t{1});

  // The toplevel's apply cascades through mid (applying mid's own cached
  // state) and on into grandchild.
  sm.commit(toplevel);
  WW_TEST_CHECK_EQ(backend.window(grandchild_win)->width, uint16_t{100});
}

WW_TEST_CASE(set_desync_flushes_cached_state_immediately_if_the_parent_is_not_sync) {
  // "If a surface's parent surface behaves as desynchronized, then the
  // cached state is applied on set_desync."
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl); // not a subsurface: never sync
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  sm.attach_buffer(child, wl.dmabuf_buffer(320, 240));
  sm.commit(child); // synchronized (default): caches
  WW_TEST_CHECK_EQ(backend.window(child_win)->width, uint16_t{1});

  // No parent commit anywhere in this test -- set_desync alone, with the
  // parent already effectively non-synchronized, is what flushes it.
  sm.set_subsurface_sync(child, false);
  WW_TEST_CHECK_EQ(backend.window(child_win)->width, uint16_t{320});
}

WW_TEST_CASE(place_above_is_double_buffered_against_the_parents_apply) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *sibling = wl.surface();
  sm.create_surface(sibling);
  sm.create_subsurface(wl.subsurface(), sibling, parent);
  const uint32_t sibling_win = backend.next_id - 1;

  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  backend.clear_calls();
  sm.place_subsurface(child, sibling, /*above=*/true);
  WW_TEST_CHECK(!backend.called("restack_window"));

  sm.commit(parent);
  WW_TEST_CHECK_EQ(backend.restacks.size(), std::size_t{1});
  if (!backend.restacks.empty()) {
    WW_TEST_CHECK_EQ(backend.restacks[0].window_id, child_win);
    WW_TEST_CHECK_EQ(backend.restacks[0].sibling_id, sibling_win);
    WW_TEST_CHECK(backend.restacks[0].above);
  }
}

WW_TEST_CASE(position_applies_on_the_parents_commit_even_for_a_desynced_child) {
  // Position and this subsurface's own buffer content are on genuinely
  // separate triggers (xdg-shell is explicit position is always timed to
  // the *parent's* apply): a desynced child's own commit applies its
  // buffer immediately, but a position set afterward without a further
  // child commit still waits on the parent.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  sm.set_subsurface_sync(child, false);
  sm.attach_buffer(child, wl.dmabuf_buffer(320, 240));
  sm.commit(child);
  WW_TEST_CHECK_EQ(backend.window(child_win)->width, uint16_t{320});

  sm.set_subsurface_position(child, 30, 40);
  WW_TEST_CHECK_EQ(backend.window(child_win)->x, int16_t{0});

  sm.commit(parent);
  WW_TEST_CHECK_EQ(backend.window(child_win)->x, int16_t{30});
}

// --- popups -----------------------------------------------------------------

WW_TEST_CASE(popup_becomes_a_child_window_of_its_parent) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  const uint32_t parent_win = backend.windows.begin()->first;

  wl_resource *popup = wl.surface();
  sm.create_surface(popup);
  sm.create_popup(popup, parent, wl.xdg_popup(), 10, 20, 100, 50);

  const uint32_t popup_win = backend.next_id - 1;
  const auto *w = backend.window(popup_win);
  WW_TEST_CHECK(w != nullptr);
  WW_TEST_CHECK_EQ(w->parent, parent_win);
  WW_TEST_CHECK_EQ(w->x, int16_t{10});
  WW_TEST_CHECK_EQ(w->width, uint16_t{100});
  WW_TEST_CHECK(w->mapped);
}

WW_TEST_CASE(a_parent_commit_does_not_move_its_popup) {
  // Popups are listed among their parent's child surfaces, but their position
  // comes from the xdg_positioner, not wl_subsurface.set_position. Applying
  // the parent's state must leave them where they were put.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *popup = wl.surface();
  sm.create_surface(popup);
  sm.create_popup(popup, parent, wl.xdg_popup(), 50, 40, 100, 80);
  const uint32_t popup_win = backend.next_id - 1;
  WW_TEST_CHECK_EQ(backend.window(popup_win)->x, int16_t{50});

  sm.commit(parent); // applies the parent's state and cascades to children

  WW_TEST_CHECK_EQ(backend.window(popup_win)->x, int16_t{50});
  WW_TEST_CHECK_EQ(backend.window(popup_win)->y, int16_t{40});
}

// --- role teardown and re-role ---------------------------------------------
// A role is torn down by assigning NoRole, so nothing from the previous role
// can leak into the next one. These pin that from the outside.

WW_TEST_CASE(subsurface_role_removed_then_reused_starts_from_initial_state) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t first_win = backend.next_id - 1;

  // Leave stale-able state behind on the first role.
  sm.set_subsurface_position(child, 30, 40);
  sm.set_subsurface_sync(child, false);
  sm.destroy_subsurface_role(child);
  WW_TEST_CHECK(backend.destroyed.count(first_win) == 1);

  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t second_win = backend.next_id - 1;
  WW_TEST_CHECK(second_win != first_win);
  WW_TEST_CHECK_EQ(backend.window(second_win)->x, int16_t{0});

  // The old pending position must not be applied to the new role.
  sm.commit(parent);
  WW_TEST_CHECK_EQ(backend.window(second_win)->x, int16_t{0});
  WW_TEST_CHECK_EQ(backend.window(second_win)->y, int16_t{0});
}

WW_TEST_CASE(a_popup_surface_can_become_a_subsurface_after_teardown) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *surf = wl.surface();
  sm.create_surface(surf);
  sm.create_popup(surf, parent, wl.xdg_popup(), 5, 6, 10, 10);
  sm.destroy_popup(surf);

  sm.create_subsurface(wl.subsurface(), surf, parent);
  WW_TEST_CHECK_EQ(backend.count("create_child_window"), std::size_t{2});
  const uint32_t win = backend.next_id - 1;
  // A subsurface's position is its own, not the popup's left-over (5,6).
  WW_TEST_CHECK_EQ(backend.window(win)->x, int16_t{0});
  WW_TEST_CHECK_EQ(backend.window(win)->y, int16_t{0});
}

WW_TEST_CASE(a_cleared_cursor_surface_can_take_another_role) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *surf = wl.surface();
  sm.create_surface(surf);
  sm.set_cursor_surface(surf, 1, 2, {});
  sm.clear_cursor_surface(surf);
  WW_TEST_CHECK_EQ(sm.cursor_for_surface(surf), uint32_t{0});

  sm.create_subsurface(wl.subsurface(), surf, parent);
  WW_TEST_CHECK_EQ(backend.count("create_child_window"), std::size_t{1});
}

WW_TEST_CASE(a_popup_cannot_become_a_toplevel) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *surf = wl.surface();
  sm.create_surface(surf);
  sm.create_popup(surf, parent, wl.xdg_popup(), 0, 0, 10, 10);
  const std::size_t windows_before = backend.windows.size();

  sm.set_toplevel(surf, wl.xdg_toplevel(), wl.xdg_surface());

  // Refused: no new window, and it is still a popup that tears down as one.
  WW_TEST_CHECK_EQ(backend.windows.size(), windows_before);
  WW_TEST_CHECK_EQ(backend.count("create_window"), std::size_t{1});
  sm.destroy_popup(surf);
  WW_TEST_CHECK(backend.destroyed.count(backend.next_id - 1) == 1);
}

WW_TEST_CASE(a_subsurface_cannot_become_a_toplevel) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *surf = wl.surface();
  sm.create_surface(surf);
  sm.create_subsurface(wl.subsurface(), surf, parent);

  sm.set_toplevel(surf, wl.xdg_toplevel(), wl.xdg_surface());

  WW_TEST_CHECK_EQ(backend.count("create_window"), std::size_t{1});
  sm.destroy_subsurface_role(surf); // still unwinds as a subsurface
  WW_TEST_CHECK(backend.destroyed.count(backend.next_id - 1) == 1);
}

WW_TEST_CASE(set_toplevel_again_on_a_toplevel_reuses_its_window) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surf = make_toplevel(sm, wl);
  sm.set_toplevel(surf, wl.xdg_toplevel(), wl.xdg_surface());

  WW_TEST_CHECK_EQ(backend.count("create_window"), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.windows.size(), std::size_t{1});
}

WW_TEST_CASE(popup_on_an_unmapped_parent_is_refused) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = wl.surface();
  sm.create_surface(parent); // no role -> no X11 window

  wl_resource *popup = wl.surface();
  sm.create_surface(popup);
  sm.create_popup(popup, parent, wl.xdg_popup(), 0, 0, 10, 10);

  // xdg-shell requires the parent be mapped first; there is no window to
  // nest into, so nothing may be created.
  WW_TEST_CHECK(!backend.called("create_child_window"));
}

WW_TEST_CASE(click_outside_dismisses_the_whole_popup_chain) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);

  wl_resource *popup1 = wl.surface();
  sm.create_surface(popup1);
  sm.create_popup(popup1, parent, wl.xdg_popup(), 0, 0, 10, 10);
  const uint32_t win1 = backend.next_id - 1;

  wl_resource *popup2 = wl.surface();
  sm.create_surface(popup2);
  sm.create_popup(popup2, popup1, wl.xdg_popup(), 0, 0, 10, 10);
  const uint32_t win2 = backend.next_id - 1;

  // Click landed on the toplevel, not on any popup.
  sm.dismiss_popups_not_owning(parent);

  WW_TEST_CHECK(backend.destroyed.count(win1) == 1);
  WW_TEST_CHECK(backend.destroyed.count(win2) == 1);
}

WW_TEST_CASE(click_on_a_popup_does_not_dismiss_the_chain) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *popup = wl.surface();
  sm.create_surface(popup);
  sm.create_popup(popup, parent, wl.xdg_popup(), 0, 0, 10, 10);
  const uint32_t win = backend.next_id - 1;

  sm.dismiss_popups_not_owning(popup);

  WW_TEST_CHECK(backend.destroyed.count(win) == 0);
}

WW_TEST_CASE(dismissing_with_no_popups_open_is_a_no_op) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  backend.clear_calls();
  sm.dismiss_popups_not_owning(parent);

  WW_TEST_CHECK(backend.calls.empty());
}

WW_TEST_CASE(destroy_popup_is_idempotent) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *popup = wl.surface();
  sm.create_surface(popup);
  sm.create_popup(popup, parent, wl.xdg_popup(), 0, 0, 10, 10);

  sm.destroy_popup(popup);
  backend.clear_calls();
  sm.destroy_popup(popup); // second teardown must do nothing at all

  WW_TEST_CHECK(backend.calls.empty());
}

WW_TEST_CASE(reposition_moves_and_resizes_a_mapped_popup) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *popup = wl.surface();
  sm.create_surface(popup);
  sm.create_popup(popup, parent, wl.xdg_popup(), 0, 0, 10, 10);
  const uint32_t win = backend.next_id - 1;

  sm.reposition_popup(popup, 5, 6, 70, 80);

  const auto *w = backend.window(win);
  WW_TEST_CHECK_EQ(w->x, int16_t{5});
  WW_TEST_CHECK_EQ(w->y, int16_t{6});
  WW_TEST_CHECK_EQ(w->width, uint16_t{70});
  WW_TEST_CHECK_EQ(w->height, uint16_t{80});
}

WW_TEST_CASE(destroying_a_popup_surface_removes_it_from_the_dismiss_stack) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *popup = wl.surface();
  sm.create_surface(popup);
  sm.create_popup(popup, parent, wl.xdg_popup(), 0, 0, 10, 10);

  // Destroy the wl_surface directly, without xdg_popup.destroy first.
  sm.destroy_surface(popup);

  // The stale entry must not survive in the stack: dismissing afterwards
  // would otherwise dereference a freed surface.
  backend.clear_calls();
  sm.dismiss_popups_not_owning(parent);
  WW_TEST_CHECK(backend.calls.empty());
}

// --- resize / close ---------------------------------------------------------

WW_TEST_CASE(request_resize_on_an_unknown_window_is_ignored) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  make_toplevel(sm, wl);
  sm.request_resize(999999, 100, 100); // no surface bound to this id
  sm.request_close(999999);
  sm.on_expose(999999);

  WW_TEST_CHECK(true); // reaching here without a crash is the assertion
}

WW_TEST_CASE(backend_resize_callback_is_wired_on_construction) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  // The manager registers its resize observer in its constructor; a backend
  // resize must therefore be routed without any further wiring.
  WW_TEST_CHECK(static_cast<bool>(backend.resize_cb));

  wl_resource *surface = make_toplevel(sm, wl);
  (void)surface;
  const uint32_t win = backend.windows.begin()->first;
  backend.resize_cb(win, 1024, 768); // must not crash; sends xdg configure
  WW_TEST_CHECK(true);
}

// --- input region -----------------------------------------------------------

WW_TEST_CASE(input_region_is_double_buffered_until_commit) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  backend.clear_calls();

  sm.set_pending_input_region(surface, {{0, 0, 10, 10}}, {}, true);
  // Per wl_surface, region state applies on commit, not when requested.
  WW_TEST_CHECK(!backend.called("set_input_shape"));

  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.count("set_input_shape"), std::size_t{1});
}

WW_TEST_CASE(committing_without_a_region_change_does_not_reshape) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_pending_input_region(surface, {{0, 0, 10, 10}}, {}, true);
  sm.commit(surface);
  backend.clear_calls();

  sm.commit(surface); // nothing changed since the last commit

  WW_TEST_CHECK(!backend.called("set_input_shape"));
}

WW_TEST_CASE(input_region_is_translated_by_the_window_geometry_origin) {
  // The regression test for a real bug introduced by window-geometry
  // cropping itself: wl_surface.set_input_region's rects are surface-local
  // (relative to the buffer's origin), same as
  // xdg_surface.set_window_geometry's rect -- but the X11 window now sits
  // at the *geometry* rect's origin, not the buffer's. A CSD client that
  // sets its input region to line up with its own window geometry (the
  // common case -- excluding a drop shadow) would otherwise get a region
  // offset from its own window by the shadow margin, leaving most or all
  // of the real window outside its own clickable area.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  // Firefox-shaped: input region matches the window geometry exactly.
  sm.set_pending_input_region(surface, {{26, 22, 1024, 731}}, {}, true);
  sm.commit(surface);

  WW_TEST_CHECK_EQ(backend.count("set_input_shape"), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.last_input_shape_add.size(), std::size_t{1});
  const auto &r = backend.last_input_shape_add.front();
  WW_TEST_CHECK_EQ(r.x, int16_t{0});
  WW_TEST_CHECK_EQ(r.y, int16_t{0});
  WW_TEST_CHECK_EQ(r.width, uint16_t{1024});
  WW_TEST_CHECK_EQ(r.height, uint16_t{731});
}

WW_TEST_CASE(input_region_subtract_rects_are_translated_too) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  // Whole buffer minus a corner, both still in buffer-local coordinates.
  sm.set_pending_input_region(surface, {{0, 0, 1076, 775}}, {{26, 22, 50, 50}},
                              true);
  sm.commit(surface);

  WW_TEST_CHECK_EQ(backend.last_input_shape_sub.size(), std::size_t{1});
  const auto &r = backend.last_input_shape_sub.front();
  WW_TEST_CHECK_EQ(r.x, int16_t{0});
  WW_TEST_CHECK_EQ(r.y, int16_t{0});
}

WW_TEST_CASE(input_region_without_window_geometry_is_untranslated) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_pending_input_region(surface, {{5, 5, 10, 10}}, {}, true);
  sm.commit(surface);

  const auto &r = backend.last_input_shape_add.front();
  WW_TEST_CHECK_EQ(r.x, int16_t{5});
  WW_TEST_CHECK_EQ(r.y, int16_t{5});
}

// Regression test: an explicitly empty input region (has_region == true,
// zero rects — the standard idiom for a purely-visual overlay/subsurface
// that wants clicks and keys to pass through to whatever is beneath it)
// must reach the backend with has_region == true, not silently collapse
// into "no region was ever declared". Before the fix, commit() dropped
// has_input_region entirely, so this and
// no_region_ever_declared_reaches_the_backend_as_such below were
// indistinguishable at the backend call.
WW_TEST_CASE(an_explicitly_empty_region_reaches_the_backend_as_such) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_pending_input_region(surface, {}, {}, true);
  sm.commit(surface);

  WW_TEST_CHECK_EQ(backend.count("set_input_shape"), std::size_t{1});
  WW_TEST_CHECK(backend.last_input_shape_has_region);
  WW_TEST_CHECK(backend.last_input_shape_add.empty());
  WW_TEST_CHECK(backend.last_input_shape_sub.empty());
}

WW_TEST_CASE(no_region_ever_declared_reaches_the_backend_as_such) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_pending_input_region(surface, {}, {}, false);
  sm.commit(surface);

  WW_TEST_CHECK_EQ(backend.count("set_input_shape"), std::size_t{1});
  WW_TEST_CHECK(!backend.last_input_shape_has_region);
  WW_TEST_CHECK(backend.last_input_shape_add.empty());
  WW_TEST_CHECK(backend.last_input_shape_sub.empty());
}

// --- client-supplied buffer dimensions ---------------------------------------

WW_TEST_CASE(degenerate_buffer_size_is_clamped_to_a_legal_window) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  // A zero-sized buffer is what an oversized one narrows to, and what a
  // buggy client can send outright. X11 rejects a zero-sized window, so the
  // commit path must clamp up to 1 rather than pass it through.
  sm.attach_buffer(child, wl.dmabuf_buffer(0, 0));
  sm.commit(child);

  const auto *w = backend.window(child_win);
  WW_TEST_CHECK(w != nullptr);
  WW_TEST_CHECK_EQ(w->width, uint16_t{1});
  WW_TEST_CHECK_EQ(w->height, uint16_t{1});
}

WW_TEST_CASE(normal_buffer_sizes_the_child_window_exactly) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  // Desynchronized: this test is about content sizing, not the sync/
  // cache-against-the-parent machinery covered separately below, so
  // taking the child out of the (default, synchronized) cached path
  // keeps its own commit applying directly.
  sm.set_subsurface_sync(child, false);
  sm.attach_buffer(child, wl.dmabuf_buffer(320, 240));
  sm.commit(child);

  const auto *w = backend.window(child_win);
  WW_TEST_CHECK_EQ(w->width, uint16_t{320});
  WW_TEST_CHECK_EQ(w->height, uint16_t{240});
}

// --- xdg_surface configure serials ------------------------------------------

WW_TEST_CASE(configure_serials_never_repeat_across_configure_sources) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;

  // set_toplevel and request_resize used to draw from two independent
  // counters that both started at 1, so a client's ack_configure could not
  // identify which configure it was acknowledging.
  sm.request_resize(win, 1024, 768);
  sm.request_resize(win, 800, 600);

  const auto serials = wl.configure_serials(xdg);
  WW_TEST_CHECK_EQ(serials.size(), std::size_t{3});
  for (std::size_t i = 1; i < serials.size(); ++i)
    WW_TEST_CHECK(serials[i] > serials[i - 1]);
}

WW_TEST_CASE(configure_serials_use_the_display_wide_sequence) {
  FakeWindowBackend backend_a;
  FakeWindowBackend backend_b;
  // Configure serials must share wl_display's sequence with popup and input
  // events, not use independent SurfaceManager-local counters.
  ww::SurfaceManager a{backend_a};
  ww::SurfaceManager b{backend_b};
  WaylandFixture wl; // destroyed before either manager -- see Harness

  wl_resource *sa = wl.surface();
  a.create_surface(sa);
  wl_resource *xa = wl.xdg_surface();
  a.set_toplevel(sa, wl.xdg_toplevel(), xa);

  wl_resource *sb = wl.surface();
  b.create_surface(sb);
  wl_resource *xb = wl.xdg_surface();
  b.set_toplevel(sb, wl.xdg_toplevel(), xb);

  const auto serials_a = wl.configure_serials(xa);
  const auto serials_b = wl.configure_serials(xb);
  WW_TEST_CHECK(!serials_a.empty());
  WW_TEST_CHECK(!serials_b.empty());
  WW_TEST_CHECK(serials_a.front() != serials_b.front());
}

// --- lifetime ---------------------------------------------------------------

WW_TEST_CASE(display_teardown_before_the_manager_is_safe) {
  // The production composition root destroys WaylandServer (and with it the
  // wl_display) before SurfaceManager. ~SurfaceManager does not unregister
  // the destroy listeners installed by set_toplevel, so resources must die
  // while the Surface behind those listeners is still alive.
  //
  // Under ASan the reverse order is a use-after-free, which is why Harness
  // fixes the declaration order rather than leaving it to each test.
  FakeWindowBackend backend;
  ww::SurfaceManager sm{backend};
  {
    WaylandFixture wl;
    wl_resource *surface = wl.surface();
    sm.create_surface(surface);
    sm.set_toplevel(surface, wl.xdg_toplevel(), wl.xdg_surface());
    sm.attach_buffer(surface, wl.dmabuf_buffer(64, 64));
    sm.commit(surface);
  } // wl_display destroyed here, firing every destroy listener

  WW_TEST_CHECK(true); // surviving teardown without a crash is the assertion
}

// --- xdg_surface.set_window_geometry ----------------------------------------
//
// A client that draws its own shadow and invisible resize border attaches a
// buffer larger than the size it was configured with, and uses
// set_window_geometry to say which sub-rectangle is the window proper.
// Ignoring it presents the shadow and displaces the real window by the
// margin. These pin down that the geometry rect, not the buffer, is what
// reaches the backend.

WW_TEST_CASE(window_geometry_selects_what_is_presented) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  sm.attach_buffer(surface, wl.dmabuf_buffer(1076, 783, /*pixmap=*/77));
  sm.commit(surface);

  WW_TEST_CHECK_EQ(backend.presented_pixmaps.size(), std::size_t{1});
  const auto r = backend.presented_regions.back();
  WW_TEST_CHECK(r.declared());
  WW_TEST_CHECK_EQ(r.x, int16_t{26});
  WW_TEST_CHECK_EQ(r.y, int16_t{22});
  WW_TEST_CHECK_EQ(r.width, uint16_t{1024});
  WW_TEST_CHECK_EQ(r.height, uint16_t{731});
}

WW_TEST_CASE(without_window_geometry_the_whole_buffer_is_presented) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/77));
  sm.commit(surface);

  // Surfaces with no xdg_surface role at all (subsurfaces, cursors) never
  // declare geometry, so "undeclared" has to keep meaning "everything".
  WW_TEST_CHECK(!backend.presented_regions.back().declared());
}

WW_TEST_CASE(window_geometry_is_clamped_to_the_buffer) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  // Geometry arrives from the client and is not required to agree with the
  // buffer it eventually attaches. A source rectangle that leaves the image
  // is a protocol error on *our* X11 connection, so it has to be clamped.
  sm.set_window_geometry(surface, 26, 22, 4000, 4000);
  sm.attach_buffer(surface, wl.dmabuf_buffer(100, 100, /*pixmap=*/77));
  sm.commit(surface);

  const auto r = backend.presented_regions.back();
  WW_TEST_CHECK_EQ(r.x, int16_t{26});
  WW_TEST_CHECK_EQ(r.y, int16_t{22});
  WW_TEST_CHECK_EQ(r.width, uint16_t{74});
  WW_TEST_CHECK_EQ(r.height, uint16_t{78});
}

WW_TEST_CASE(window_geometry_entirely_outside_the_buffer_presents_everything) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 500, 500, 100, 100);
  sm.attach_buffer(surface, wl.dmabuf_buffer(100, 100, /*pixmap=*/77));
  sm.commit(surface);

  // Nothing sensible to crop to: fall back to the whole buffer rather than
  // presenting a zero-sized rectangle, i.e. a blank window.
  WW_TEST_CHECK(!backend.presented_regions.back().declared());
}

WW_TEST_CASE(window_geometry_is_double_buffered_until_commit) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  sm.attach_buffer(surface, wl.dmabuf_buffer(1076, 783, /*pixmap=*/77));

  // Nothing has reached the window yet -- surface_local_origin() (used by
  // the input layer for pointer/drag correction, see its own tests) has to
  // agree with this, not just presentation below.
  int32_t ox = -1, oy = -1;
  sm.surface_local_origin(surface, ox, oy);
  WW_TEST_CHECK_EQ(ox, int32_t{0});
  WW_TEST_CHECK_EQ(oy, int32_t{0});

  sm.commit(surface);
  const auto r = backend.presented_regions.back();
  WW_TEST_CHECK(r.declared());
  WW_TEST_CHECK_EQ(r.x, int16_t{26});
  WW_TEST_CHECK_EQ(r.y, int16_t{22});
  sm.surface_local_origin(surface, ox, oy);
  WW_TEST_CHECK_EQ(ox, int32_t{26});
  WW_TEST_CHECK_EQ(oy, int32_t{22});
}

WW_TEST_CASE(window_geometry_persists_across_a_commit_that_declares_none) {
  // A client sets geometry once and keeps committing new frames after --
  // it does not have to repeat set_window_geometry on every single one for
  // the crop to keep applying (this is what PendingState::window_geometry's gating is
  // for: unlike offset, geometry is a sticky value, not a per-commit one).
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  sm.attach_buffer(surface, wl.dmabuf_buffer(1076, 783, /*pixmap=*/77));
  sm.commit(surface);
  WW_TEST_CHECK(backend.presented_regions.back().declared());

  sm.attach_buffer(surface, wl.dmabuf_buffer(1076, 783, /*pixmap=*/78));
  sm.commit(surface);
  const auto r = backend.presented_regions.back();
  WW_TEST_CHECK(r.declared());
  WW_TEST_CHECK_EQ(r.x, int16_t{26});
  WW_TEST_CHECK_EQ(r.y, int16_t{22});
}

WW_TEST_CASE(a_zero_sized_geometry_clears_it) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  sm.set_window_geometry(surface, 0, 0, 0, 0);
  sm.attach_buffer(surface, wl.dmabuf_buffer(1076, 783, /*pixmap=*/77));
  sm.commit(surface);

  WW_TEST_CHECK(!backend.presented_regions.back().declared());
}

// --- wl_surface.offset ------------------------------------------------------
//
// A client growing its own shadow on one side (a top-left interactive
// resize, say) attaches a bigger buffer and reports an offset so the
// *visible* window doesn't appear to jump: the window_geometry rect it
// re-declares describes the same on-screen rectangle as before (this
// project's compositor never moves the X11 window for this -- see
// Surface::offset_{x,y}'s doc comment -- so window_geometry's
// surface-local coordinates for the "real window" portion are unchanged
// too), but that rectangle now sits at a different position *within* the
// bigger buffer, which is exactly what these pin down.

WW_TEST_CASE(offset_shifts_where_geometry_is_cropped_from_in_the_buffer) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  sm.attach_buffer(surface, wl.dmabuf_buffer(1076, 783, /*pixmap=*/77));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_regions.back().x, int16_t{26});

  // The client grows the buffer by 10px on the left (its shadow got
  // bigger there) and reports that as an offset of -10: the new buffer's
  // upper-left corner is 10px to the *left*, in surface-local terms, of
  // where the old one was. The re-declared geometry rect is unchanged
  // (26, 22) -- surface-local, and the real window hasn't moved.
  sm.set_pending_offset(surface, -10, 0);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  sm.attach_buffer(surface, wl.dmabuf_buffer(1086, 783, /*pixmap=*/78));
  sm.commit(surface);

  const auto r = backend.presented_regions.back();
  // 26 - (-10): what was at surface-local x=26 is now 10px further into
  // the wider buffer.
  WW_TEST_CHECK_EQ(r.x, int16_t{36});
  WW_TEST_CHECK_EQ(r.y, int16_t{22});
  WW_TEST_CHECK_EQ(r.width, uint16_t{1024});
  WW_TEST_CHECK_EQ(r.height, uint16_t{731});
}

WW_TEST_CASE(offset_accumulates_across_commits_that_declare_no_new_offset) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  sm.set_pending_offset(surface, -10, 0);
  sm.attach_buffer(surface, wl.dmabuf_buffer(1086, 783, /*pixmap=*/77));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_regions.back().x, int16_t{36});

  // No new offset() call this time -- the running total from the previous
  // commit must still apply, not reset to zero.
  sm.attach_buffer(surface, wl.dmabuf_buffer(1086, 783, /*pixmap=*/78));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_regions.back().x, int16_t{36});
}

WW_TEST_CASE(a_second_offset_before_commit_replaces_the_first_not_stacks) {
  // Both wl_surface.offset and wl_surface.attach's own (deprecated) x/y
  // describe "the new buffer's corner relative to the currently
  // *committed* one" -- calling either twice before a commit describes
  // the same new buffer twice, so the second call's value is what counts,
  // not the sum of both.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  sm.set_pending_offset(surface, -3, 0);
  sm.set_pending_offset(surface, -10, 0);
  sm.attach_buffer(surface, wl.dmabuf_buffer(1086, 783, /*pixmap=*/77));
  sm.commit(surface);

  WW_TEST_CHECK_EQ(backend.presented_regions.back().x, int16_t{36}); // 26-(-10)
}

WW_TEST_CASE(offset_also_shifts_the_translated_input_region) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 26, 22, 1024, 731);
  sm.set_pending_offset(surface, -10, 0);
  // Input region matches window geometry exactly, in the *new*, wider
  // buffer's own coordinates (36 = 26 - (-10), same relationship as the
  // geometry crop above).
  sm.set_pending_input_region(surface, {{36, 22, 1024, 731}}, {}, true);
  sm.attach_buffer(surface, wl.dmabuf_buffer(1086, 783, /*pixmap=*/77));
  sm.commit(surface);

  const auto &r = backend.last_input_shape_add.front();
  WW_TEST_CHECK_EQ(r.x, int16_t{0});
  WW_TEST_CHECK_EQ(r.y, int16_t{0});
  WW_TEST_CHECK_EQ(r.width, uint16_t{1024});
  WW_TEST_CHECK_EQ(r.height, uint16_t{731});
}

WW_TEST_CASE(offset_with_no_window_geometry_does_not_shift_presentation) {
  // Without set_window_geometry, the whole buffer is the window (see
  // without_window_geometry_the_whole_buffer_is_presented) -- there is no
  // other surface-local-anchored state here for the offset to correct
  // against, so this is deliberately not applied in this case (see
  // buffer_region()'s doc comment).
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_pending_offset(surface, -10, -5);
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/77));
  sm.commit(surface);

  WW_TEST_CHECK(!backend.presented_regions.back().declared());
}

WW_TEST_CASE(a_child_window_is_sized_to_its_geometry_not_its_buffer) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  // Desynchronized -- see normal_buffer_sizes_the_child_window_exactly's
  // own comment; same reasoning.
  sm.set_subsurface_sync(child, false);

  // A menu with a drop shadow: 216x116 of pixels, 200x100 of window.
  sm.set_window_geometry(child, 8, 8, 200, 100);
  sm.attach_buffer(child, wl.dmabuf_buffer(216, 116));
  sm.commit(child);

  const auto *w = backend.window(child_win);
  WW_TEST_CHECK_EQ(w->width, uint16_t{200});
  WW_TEST_CHECK_EQ(w->height, uint16_t{100});
}

WW_TEST_CASE(subsurface_position_is_relative_to_the_parents_geometry_origin) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  sm.set_window_geometry(parent, 26, 22, 1024, 731);
  sm.commit(parent); // double-buffered against commit -- see PendingState::window_geometry

  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  // set_position is in the parent *surface's* coordinates, but the X11
  // child hangs off a window whose top-left is the parent's geometry
  // origin. The difference is the parent's shadow margin.
  sm.set_subsurface_position(child, 30, 40);
  sm.commit(parent); // position is gated on the parent's apply, not this call

  WW_TEST_CHECK_EQ(backend.windows.at(child_win).x, int16_t{4});
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).y, int16_t{18});
}

WW_TEST_CASE(moving_the_geometry_origin_moves_existing_children) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;
  sm.set_subsurface_position(child, 30, 40);
  sm.commit(parent); // position is gated on the parent's apply, not set_position
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).x, int16_t{30});

  // Geometry usually arrives before any child exists, but a client may
  // resize and re-declare it at any point; children have to follow.
  sm.set_window_geometry(parent, 26, 22, 1024, 731);
  sm.commit(parent); // double-buffered against commit -- see PendingState::window_geometry

  WW_TEST_CHECK_EQ(backend.windows.at(child_win).x, int16_t{4});
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).y, int16_t{18});
}

WW_TEST_CASE(a_new_geometry_does_not_move_children_before_the_parent_commits) {
  // The double-buffering point made concrete: reposition_children() has to
  // wait for the parent's own commit, or a child would jump to the new
  // geometry origin while the parent's own buffer -- and the crop that
  // origin is measured against -- is still the old one on screen.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;
  sm.set_subsurface_position(child, 30, 40);
  sm.commit(parent); // position is gated on the parent's apply, not set_position
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).x, int16_t{30});

  sm.set_window_geometry(parent, 26, 22, 1024, 731);
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).x, int16_t{30});
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).y, int16_t{40});

  sm.commit(parent);
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).x, int16_t{4});
  WW_TEST_CHECK_EQ(backend.windows.at(child_win).y, int16_t{18});
}

// --- configure deduplication ------------------------------------------------

WW_TEST_CASE(an_identical_configure_is_not_sent_twice) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;

  // One window-manager decision routinely produces several identical
  // ConfigureNotify events (reparent, then fit-to-workarea).
  sm.request_resize(win, 1024, 731);
  sm.request_resize(win, 1024, 731);
  sm.request_resize(win, 1024, 731);

  // set_toplevel's initial 800x600 configure, then one for 1024x731.
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), std::size_t{2});
}

WW_TEST_CASE(duplicate_resize_events_never_resend_a_configure) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = h.backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/70));
  sm.commit(surface);
  const std::size_t before_resize = wl.configure_serials(xdg).size();

  sm.request_resize(win, 1024, 768);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before_resize + 1);
  sm.request_resize(win, 1024, 768);
  sm.request_resize(win, 1024, 768);
  h.timer.advance(std::chrono::milliseconds{250});
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before_resize + 1);
}

WW_TEST_CASE(expose_repaints_stale_content_without_resending_configure) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/69));
  sm.commit(surface);
  const std::size_t before_resize = wl.configure_serials(xdg).size();

  sm.request_resize(win, 1024, 768);
  const std::size_t after_resize = wl.configure_serials(xdg).size();
  WW_TEST_CHECK_EQ(after_resize, before_resize + 1);
  sm.on_expose(win);
  sm.on_expose(win);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), after_resize);
  WW_TEST_CHECK_EQ(backend.count("repaint_window_scaled"), std::size_t{2});
  WW_TEST_CHECK_EQ(backend.last_stretch_size,
                   (std::pair<uint16_t, uint16_t>{1024, 768}));
}

WW_TEST_CASE(first_wm_target_starts_a_fresh_settle_deadline) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/76));
  sm.commit(surface); // First commit adopts 640x480.
  const std::size_t resizes_after_first_commit = backend.count("resize_window");

  // Before a WM target has actually been observed, a subsequent client commit
  // must not settle against the provisional 800x600 initial configure.
  sm.attach_buffer(surface, wl.dmabuf_buffer(700, 530, /*pixmap=*/761));
  sm.commit(surface);
  h.timer.advance(std::chrono::milliseconds{1000});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_after_first_commit);

  // Time spent before the first WM ConfigureNotify must not count toward its
  // settle delay just because the provisional initial size was 800x600.
  sm.request_resize(win, 800, 600); // Same as provisional size, first WM report.
  const auto serial = wl.configure_serials(xdg).front();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, serial));
  const std::size_t resizes_after_target = backend.count("resize_window");
  h.timer.advance(std::chrono::milliseconds{249});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_after_target);
  h.timer.advance(std::chrono::milliseconds{1});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_after_target + 1);
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{700});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{530});
}

WW_TEST_CASE(stale_commits_never_resend_configure_and_wait_for_ack_and_settle) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;

  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/77));
  sm.commit(surface);
  const std::size_t before_resize = wl.configure_serials(xdg).size();
  sm.request_resize(win, 1024, 768);
  const uint32_t configure_serial = wl.configure_serials(xdg).back();
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before_resize + 1);

  // Ack the WM configure, then submit repeated stale-size commits. They do
  // not trigger another configure, and the timer must not adopt until the
  // original target has been stable for 250 ms.
  WW_TEST_CHECK(sm.acknowledge_configure(surface, configure_serial));
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/78));
  sm.commit(surface);
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/79));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before_resize + 1);
  WW_TEST_CHECK(backend.called("present_pixmap_scaled"));
  const std::size_t resizes_after_commit = backend.count("resize_window");
  h.timer.advance(std::chrono::milliseconds{249});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_after_commit);
  h.timer.advance(std::chrono::milliseconds{1});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_after_commit + 1);
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{640});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{480});
  // The timer adoption must also replay the backend-retained dmabuf image at
  // its committed size; it cannot leave the interim scaled cache cropped.
  WW_TEST_CHECK(backend.called("repaint_window"));
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before_resize + 1);
}

WW_TEST_CASE(late_configure_ack_adopts_existing_commit_without_a_new_commit) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/87));
  sm.commit(surface);
  const std::size_t resizes_before = backend.count("resize_window");

  sm.request_resize(win, 1024, 768);
  const uint32_t configure_serial = wl.configure_serials(xdg).back();
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/88));
  sm.commit(surface);
  h.timer.advance(std::chrono::milliseconds{250});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_before);

  // The timer expired before the client acknowledged, but after the ack the
  // already-committed geometry is still eligible: no extra buffer commit is
  // needed merely to make the timer re-evaluate it.
  WW_TEST_CHECK(sm.acknowledge_configure(surface, configure_serial));
  h.timer.advance(std::chrono::milliseconds{0});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_before + 1);
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{640});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{480});
  const std::size_t configure_count = wl.configure_serials(xdg).size();
  sm.request_resize(win, 640, 480);
  sm.request_resize(win, 640, 480);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), configure_count);
}

WW_TEST_CASE(later_commit_matching_wm_target_restores_target_size_after_adoption) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/89));
  sm.commit(surface);

  sm.request_resize(win, 1024, 768);
  const auto serial = wl.configure_serials(xdg).back();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, serial));
  h.timer.advance(std::chrono::milliseconds{250});
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{640});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{480});

  // The client may still finish a delayed buffer at the requested size after
  // the settle timer adopted an earlier grid-quantized frame. That new frame
  // is not stale; the floating X11 window must return to the WM target.
  sm.attach_buffer(surface, wl.dmabuf_buffer(1024, 768, /*pixmap=*/90));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{1024});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{768});

  // The X11 ConfigureNotify for the earlier 640x480 resize may be delayed
  // until after this newer 1024x768 resize has been requested. It must not
  // roll the tracked WM target backward or cause a redundant configure.
  const std::size_t configures_after_restore = wl.configure_serials(xdg).size();
  sm.request_resize(win, 640, 480);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), configures_after_restore);
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{1024});
  sm.request_resize(win, 1000, 800);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), configures_after_restore + 1);
}

WW_TEST_CASE(resize_settle_deadline_restarts_when_wm_target_changes) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/82));
  sm.commit(surface);
  const std::size_t resizes_before = backend.count("resize_window");

  sm.request_resize(win, 900, 700);
  auto serial = wl.configure_serials(xdg).back();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, serial));
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/83));
  sm.commit(surface);
  h.timer.advance(std::chrono::milliseconds{200});

  sm.request_resize(win, 1000, 800);
  serial = wl.configure_serials(xdg).back();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, serial));
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/84));
  sm.commit(surface);
  h.timer.advance(std::chrono::milliseconds{50});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_before);
  h.timer.advance(std::chrono::milliseconds{199});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_before);
  h.timer.advance(std::chrono::milliseconds{1});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_before + 1);
}

WW_TEST_CASE(nv12_commits_receive_the_same_interim_stretch_target) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;

  sm.attach_buffer(surface, wl.nv12_buffer(640, 480));
  sm.commit(surface); // First frame establishes the client's current size.
  sm.request_resize(win, 1024, 768);
  const auto serial = wl.configure_serials(xdg).back();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, serial));
  sm.attach_buffer(surface, wl.nv12_buffer(640, 480));
  sm.commit(surface);

  WW_TEST_CHECK(backend.called("present_nv12_scaled"));
  WW_TEST_CHECK_EQ(backend.last_stretch_size,
                   (std::pair<uint16_t, uint16_t>{1024, 768}));
}

WW_TEST_CASE(a_tiled_client_that_resizes_itself_is_told_the_tile_size_again) {
  // foot with resize-keep-grid: on a font zoom it commits a buffer of a new
  // size (the grid it wants) even though it is tiled. The window manager owns
  // the geometry, so WWindows can never adopt that size; the client has to be
  // told the tile size once more or it stays at its own size, stretched, until
  // some unrelated layout change happens to send it a configure.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = backend.windows.begin()->first;

  sm.set_tiled(win, true);
  sm.request_resize(win, 1024, 768);
  WW_TEST_CHECK(sm.acknowledge_configure(surface, wl.configure_serials(xdg).back()));
  // The client obeys the configure: committed size == tile size.
  sm.attach_buffer(surface, wl.dmabuf_buffer(1024, 768, /*pixmap=*/90));
  sm.commit(surface);
  const std::size_t baseline = wl.configure_serials(xdg).size();
  const std::size_t resizes_before = backend.count("resize_window");

  // Zoom: the client commits a different size by itself.
  sm.attach_buffer(surface, wl.dmabuf_buffer(512, 384, /*pixmap=*/91));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), baseline + 1);
  WW_TEST_CHECK(wl.last_toplevel_size(toplevel) ==
                (std::pair<int32_t, int32_t>{1024, 768}));
  // The window manager still owns the geometry: nothing was adopted.
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_before);

  // A client that keeps committing the same wrong size is not nagged: one
  // re-assert per distinct size, so a client that cannot comply cannot start
  // a configure/commit loop.
  WW_TEST_CHECK(sm.acknowledge_configure(surface, wl.configure_serials(xdg).back()));
  sm.attach_buffer(surface, wl.dmabuf_buffer(512, 384, /*pixmap=*/92));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), baseline + 1);

  // A different wrong size (the next zoom step) is re-asserted again.
  sm.attach_buffer(surface, wl.dmabuf_buffer(256, 192, /*pixmap=*/93));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), baseline + 2);

  // Once the client obeys, the memory resets: the next deviation counts again.
  WW_TEST_CHECK(sm.acknowledge_configure(surface, wl.configure_serials(xdg).back()));
  sm.attach_buffer(surface, wl.dmabuf_buffer(1024, 768, /*pixmap=*/94));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), baseline + 2);
  sm.attach_buffer(surface, wl.dmabuf_buffer(512, 384, /*pixmap=*/95));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), baseline + 3);
}

WW_TEST_CASE(a_floating_client_that_resizes_itself_is_not_reconfigured) {
  // The counterpart: a floating window adopts the client's size (after the
  // settle delay), so nothing needs to be re-asserted. This is the behaviour
  // stale_commits_never_resend_configure_and_wait_for_ack_and_settle pins;
  // here with the client-driven shrink that a zoom produces.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(2), xdg);
  const uint32_t win = backend.windows.begin()->first;

  sm.request_resize(win, 1024, 768);
  WW_TEST_CHECK(sm.acknowledge_configure(surface, wl.configure_serials(xdg).back()));
  sm.attach_buffer(surface, wl.dmabuf_buffer(1024, 768, /*pixmap=*/96));
  sm.commit(surface);
  const std::size_t baseline = wl.configure_serials(xdg).size();

  sm.attach_buffer(surface, wl.dmabuf_buffer(512, 384, /*pixmap=*/97));
  sm.commit(surface);
  h.timer.advance(std::chrono::milliseconds{250});
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), baseline);
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{512});
}

WW_TEST_CASE(wm_owned_geometry_stays_stretched_and_is_not_adopted) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/85));
  sm.commit(surface);
  sm.set_tiled(win, true);
  sm.request_resize(win, 1024, 768);
  const uint32_t configure_serial = wl.configure_serials(xdg).back();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, configure_serial));
  const std::size_t resizes_before = backend.count("resize_window");
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/86));
  sm.commit(surface);
  WW_TEST_CHECK(backend.called("present_pixmap_scaled"));
  h.timer.advance(std::chrono::milliseconds{500});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_before);
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{640});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{480});
}

WW_TEST_CASE(external_fullscreen_and_partial_maximize_keep_geometry_wm_owned) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/190));
  sm.commit(surface);

  // The user enters fullscreen through the WM, not through an
  // xdg_toplevel.set_fullscreen request. This must still prevent geometry
  // adoption while preserving the scale-to-target presentation policy.
  sm.set_wm_state(win, {/*horz=*/false, /*vert=*/false, /*fullscreen=*/true});
  sm.request_resize(win, 1024, 768);
  const uint32_t fullscreen_serial = wl.configure_serials(xdg).back();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, fullscreen_serial));
  const std::size_t resizes_before_stale_commit =
      backend.count("resize_window");
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/191));
  sm.commit(surface);
  WW_TEST_CHECK(backend.called("present_pixmap_scaled"));
  h.timer.advance(std::chrono::milliseconds{500});
  WW_TEST_CHECK_EQ(backend.count("resize_window"),
                   resizes_before_stale_commit);

  // One-axis maximize/edge snapping is also WM-owned, although it must not
  // be advertised as the four-edge XDG tiled state.
  sm.set_wm_state(win, {/*horz=*/true, /*vert=*/false, /*fullscreen=*/false});
  h.timer.advance(std::chrono::milliseconds{500});
  WW_TEST_CHECK_EQ(backend.count("resize_window"),
                   resizes_before_stale_commit);
}

WW_TEST_CASE(leaving_external_wm_owned_geometry_starts_a_fresh_settle_delay) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/192));
  sm.commit(surface);

  sm.set_wm_state(win, {/*horz=*/false, /*vert=*/false, /*fullscreen=*/true});
  sm.request_resize(win, 1024, 768);
  const uint32_t serial = wl.configure_serials(xdg).back();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, serial));
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/193));
  sm.commit(surface);
  // The WM owns the size, so the client's 640x480 is answered with the size
  // again; a real client acknowledges that promptly.
  WW_TEST_CHECK(sm.acknowledge_configure(surface, wl.configure_serials(xdg).back()));
  const std::size_t resizes_before = backend.count("resize_window");

  // After remaining fullscreen beyond the first settle deadline, leaving
  // fullscreen must establish a new full 250 ms floating settle interval.
  h.timer.advance(std::chrono::milliseconds{500});
  sm.set_wm_state(win, {/*horz=*/false, /*vert=*/false, /*fullscreen=*/false});
  // Leaving fullscreen is reported to the client (a configure without the
  // FULLSCREEN state), which acknowledges it like any other.
  WW_TEST_CHECK(sm.acknowledge_configure(surface, wl.configure_serials(xdg).back()));
  h.timer.advance(std::chrono::milliseconds{249});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_before);
  h.timer.advance(std::chrono::milliseconds{1});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), resizes_before + 1);
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{640});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{480});
}

WW_TEST_CASE(damage_only_commit_repaints_the_retained_image) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/91));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.count("present_pixmap"), std::size_t{1});

  // Damage-only commits must reuse the backend-owned snapshot. They must not
  // attempt to import or re-read a client buffer which may have been reused.
  sm.mark_surface_damage(surface, 10, 10);
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.count("repaint_window"), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.count("present_pixmap"), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.repainted_windows_with_frame.size(),
                   std::size_t{1});

  // Empty damage rectangles are a no-op.
  sm.mark_surface_damage(surface, 0, 10);
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.count("repaint_window"), std::size_t{1});
}

WW_TEST_CASE(a_state_change_at_an_unchanged_size_still_configures) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel();
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = backend.windows.begin()->first;

  sm.request_resize(win, 1024, 731);
  // Deduplicating on size alone would swallow this: same size, different
  // states. The client would never learn it had been maximized, and would
  // go on drawing a shadow it no longer has room for. The window manager,
  // not the client's request, is what says the window is maximized.
  sm.set_wm_state(win, {/*horz=*/true, /*vert=*/true, /*fullscreen=*/false});

  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), std::size_t{3});
  const auto states = wl.last_toplevel_states(toplevel);
  WW_TEST_CHECK_EQ(states.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(states.front(),
                   uint32_t{XDG_TOPLEVEL_STATE_MAXIMIZED});
}

// --- window-manager tiling hints --------------------------------------------

namespace {

bool has_state(const std::vector<uint32_t> &states, uint32_t s) {
  return std::find(states.begin(), states.end(), s) != states.end();
}

} // namespace

// --- window-manager-agnostic (EWMH) tiling hints ----------------------------

// --- keyboard focus -> xdg_toplevel ACTIVATED ---------------------------------

WW_TEST_CASE(the_focused_toplevel_is_told_it_is_activated) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  WW_TEST_CHECK(!has_state(wl.last_toplevel_states(toplevel),
                           XDG_TOPLEVEL_STATE_ACTIVATED));
  const std::size_t before = wl.configure_serials(xdg).size();

  sm.set_activated_surface(surface);

  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before + 1);
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(toplevel),
                          XDG_TOPLEVEL_STATE_ACTIVATED));
  // Same size, only the states changed.
  WW_TEST_CHECK(wl.last_toplevel_size(toplevel) ==
                (std::pair<int32_t, int32_t>{800, 600}));
}

WW_TEST_CASE(focus_moving_between_toplevels_moves_the_activated_state) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *a = wl.surface();
  wl_resource *a_xdg = wl.xdg_surface();
  wl_resource *a_top = wl.xdg_toplevel(2);
  sm.create_surface(a);
  sm.set_toplevel(a, a_top, a_xdg);
  wl_resource *b = wl.surface();
  wl_resource *b_xdg = wl.xdg_surface();
  wl_resource *b_top = wl.xdg_toplevel(2);
  sm.create_surface(b);
  sm.set_toplevel(b, b_top, b_xdg);

  sm.set_activated_surface(a);
  sm.set_activated_surface(b);

  WW_TEST_CHECK(!has_state(wl.last_toplevel_states(a_top),
                           XDG_TOPLEVEL_STATE_ACTIVATED));
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(b_top),
                          XDG_TOPLEVEL_STATE_ACTIVATED));

  // Focus leaving to something that is not ours deactivates everything.
  sm.set_activated_surface(nullptr);
  WW_TEST_CHECK(!has_state(wl.last_toplevel_states(b_top),
                           XDG_TOPLEVEL_STATE_ACTIVATED));
}

WW_TEST_CASE(repeating_the_same_focus_sends_no_further_configure) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(2), xdg);

  sm.set_activated_surface(surface);
  const std::size_t after_first = wl.configure_serials(xdg).size();
  sm.set_activated_surface(surface);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), after_first);
}

WW_TEST_CASE(focus_on_a_popup_keeps_its_toplevel_activated) {
  // A menu with a grab takes keyboard focus, but the window the user is in is
  // still the toplevel that owns it. Deactivating it would tell the client
  // it lost focus the moment its own menu opened.
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *parent = wl.surface();
  wl_resource *parent_xdg = wl.xdg_surface();
  wl_resource *parent_top = wl.xdg_toplevel(2);
  sm.create_surface(parent);
  sm.set_toplevel(parent, parent_top, parent_xdg);
  sm.set_activated_surface(parent);
  const std::size_t configures = wl.configure_serials(parent_xdg).size();

  wl_resource *popup = wl.surface();
  sm.create_surface(popup);
  sm.create_popup(popup, parent, wl.xdg_popup(), 0, 0, 10, 10);
  sm.set_activated_surface(popup);

  WW_TEST_CHECK(has_state(wl.last_toplevel_states(parent_top),
                          XDG_TOPLEVEL_STATE_ACTIVATED));
  WW_TEST_CHECK_EQ(wl.configure_serials(parent_xdg).size(), configures);

  // Nested popup: still the same root toplevel.
  wl_resource *nested = wl.surface();
  sm.create_surface(nested);
  sm.create_popup(nested, popup, wl.xdg_popup(), 0, 0, 10, 10);
  sm.set_activated_surface(nested);
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(parent_top),
                          XDG_TOPLEVEL_STATE_ACTIVATED));

  // Focus leaving to something that is not ours still deactivates it.
  sm.set_activated_surface(nullptr);
  WW_TEST_CHECK(!has_state(wl.last_toplevel_states(parent_top),
                           XDG_TOPLEVEL_STATE_ACTIVATED));
}

WW_TEST_CASE(focus_on_a_subsurface_keeps_its_toplevel_activated) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *parent = wl.surface();
  wl_resource *parent_xdg = wl.xdg_surface();
  wl_resource *parent_top = wl.xdg_toplevel(2);
  sm.create_surface(parent);
  sm.set_toplevel(parent, parent_top, parent_xdg);

  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  sm.set_activated_surface(child);

  WW_TEST_CHECK(has_state(wl.last_toplevel_states(parent_top),
                          XDG_TOPLEVEL_STATE_ACTIVATED));
}

WW_TEST_CASE(focus_on_a_surface_with_no_toplevel_above_it_activates_nothing) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *toplevel_surface = make_toplevel(sm, wl);
  wl_resource *orphan = wl.surface();
  sm.create_surface(orphan);
  sm.set_activated_surface(toplevel_surface);
  sm.set_activated_surface(orphan); // a role-less surface: nothing to activate
  // The previously active toplevel is deactivated, and nothing crashes.
}

WW_TEST_CASE(both_axes_maximized_is_reported_as_tiled) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  sm.set_wm_state(win, {/*horz=*/true, /*vert=*/true, /*fullscreen=*/false});
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(toplevel),
                          XDG_TOPLEVEL_STATE_TILED_LEFT));
}

WW_TEST_CASE(maximized_on_only_one_axis_is_not_reported_as_tiled) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  // A window snapped to one screen half: only one axis is full extent.
  sm.set_wm_state(win, {/*horz=*/true, /*vert=*/false, /*fullscreen=*/false});
  WW_TEST_CHECK(wl.last_toplevel_states(toplevel).empty());
  sm.set_wm_state(win, {/*horz=*/false, /*vert=*/true, /*fullscreen=*/false});
  WW_TEST_CHECK(wl.last_toplevel_states(toplevel).empty());
}

WW_TEST_CASE(wm_fullscreen_alongside_both_maximize_atoms_is_not_tiled) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  sm.set_wm_state(win, {/*horz=*/true, /*vert=*/true, /*fullscreen=*/true});
  WW_TEST_CHECK(!has_state(wl.last_toplevel_states(toplevel),
                           XDG_TOPLEVEL_STATE_TILED_LEFT));
}

WW_TEST_CASE(clearing_maximize_state_clears_tiled) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  sm.set_wm_state(win, {true, true, false});
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(toplevel),
                          XDG_TOPLEVEL_STATE_TILED_LEFT));
  // The PROPERTY_DELETE path in the X11 adapter reports this same all-false
  // state when a window manager withdraws _NET_WM_STATE entirely.
  sm.set_wm_state(win, {false, false, false});
  WW_TEST_CHECK(wl.last_toplevel_states(toplevel).empty());
}

WW_TEST_CASE(backend_wm_state_callback_is_wired_on_construction) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  // Mirrors backend_resize_callback_is_wired_on_construction: the manager
  // registers this observer too, so a backend that discovers _NET_WM_STATE
  // (or an equivalent) needs no further wiring from the composition root.
  WW_TEST_CHECK(static_cast<bool>(backend.wm_state_cb));

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = backend.windows.begin()->first;

  backend.wm_state_cb(win, {true, true, false});
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(toplevel),
                          XDG_TOPLEVEL_STATE_TILED_LEFT));
}

WW_TEST_CASE(a_tiled_toplevel_is_told_so_even_at_an_unchanged_size) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = backend.windows.begin()->first;

  sm.request_resize(win, 1024, 731);
  const auto before = wl.configure_serials(xdg).size();
  const auto resizes_before = std::count(backend.calls.begin(),
                                         backend.calls.end(), "resize_window");

  // i3 tiles a window into the rect it already had: no dimension changes,
  // and a size-only comparison would swallow this configure.
  sm.set_tiled(win, true);

  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before + 1);
  const auto states = wl.last_toplevel_states(toplevel);
  WW_TEST_CHECK(has_state(states, XDG_TOPLEVEL_STATE_TILED_LEFT));
  WW_TEST_CHECK(has_state(states, XDG_TOPLEVEL_STATE_TILED_RIGHT));
  WW_TEST_CHECK(has_state(states, XDG_TOPLEVEL_STATE_TILED_TOP));
  WW_TEST_CHECK(has_state(states, XDG_TOPLEVEL_STATE_TILED_BOTTOM));

  // The window manager keeps the geometry: telling the client it is tiled
  // must never make us resize the X11 window ourselves.
  WW_TEST_CHECK_EQ(std::count(backend.calls.begin(), backend.calls.end(),
                              "resize_window"),
                   resizes_before);

  // Floating again clears them, again with no size change.
  sm.set_tiled(win, false);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before + 2);
  WW_TEST_CHECK(wl.last_toplevel_states(toplevel).empty());
}

WW_TEST_CASE(repeating_the_same_tiling_state_does_not_reconfigure) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(2), xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  sm.set_tiled(win, true);
  const auto after_first = wl.configure_serials(xdg).size();
  sm.set_tiled(win, true);
  sm.set_tiled(win, true);
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), after_first);
}

WW_TEST_CASE(a_version_1_client_is_never_sent_tiled_states) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(1);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  const auto before = wl.configure_serials(xdg).size();
  sm.set_tiled(win, true);
  // The states the client would see are unchanged, so nothing is sent.
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before);
  WW_TEST_CHECK(wl.last_toplevel_states(toplevel).empty());
}

WW_TEST_CASE(a_fullscreen_toplevel_is_not_also_tiled) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  sm.set_tiled(win, true);
  sm.set_toplevel_state(surface, ww::WindowState::Fullscreen, true);
  sm.set_wm_state(win, {/*horz=*/false, /*vert=*/false, /*fullscreen=*/true});

  const auto states = wl.last_toplevel_states(toplevel);
  WW_TEST_CHECK(has_state(states, XDG_TOPLEVEL_STATE_FULLSCREEN));
  WW_TEST_CHECK(!has_state(states, XDG_TOPLEVEL_STATE_TILED_LEFT));
}

// --- fullscreen / maximized are the window manager's to report ----------------

WW_TEST_CASE(a_fullscreen_request_is_answered_but_not_claimed_until_the_wm_does) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = backend.windows.begin()->first;
  const std::size_t before = wl.configure_serials(xdg).size();

  sm.set_toplevel_state(surface, ww::WindowState::Fullscreen, true);

  // Forwarded to the WM, and xdg-shell has the compositor answer with a
  // configure -- but one that does not yet say FULLSCREEN: the WM has not
  // done anything, and may refuse.
  WW_TEST_CHECK_EQ(backend.window_states.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before + 1);
  WW_TEST_CHECK(!has_state(wl.last_toplevel_states(toplevel),
                           XDG_TOPLEVEL_STATE_FULLSCREEN));

  // Once the WM has actually fullscreened the window, the client is told.
  sm.set_wm_state(win, {false, false, /*fullscreen=*/true});
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(toplevel),
                          XDG_TOPLEVEL_STATE_FULLSCREEN));
}

WW_TEST_CASE(a_window_the_wm_fullscreens_on_its_own_is_reported_to_the_client) {
  // i3's `fullscreen toggle`: no xdg_toplevel request ever passes through.
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  sm.set_wm_state(win, {false, false, /*fullscreen=*/true});
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(toplevel),
                          XDG_TOPLEVEL_STATE_FULLSCREEN));

  sm.set_wm_state(win, {false, false, /*fullscreen=*/false});
  WW_TEST_CHECK(!has_state(wl.last_toplevel_states(toplevel),
                           XDG_TOPLEVEL_STATE_FULLSCREEN));
}

WW_TEST_CASE(after_the_wm_leaves_fullscreen_the_next_request_is_not_swallowed) {
  // The client asked for fullscreen, the WM obliged, then the user left
  // fullscreen with the WM's own binding. The client's next request (F11
  // again) must reach the WM instead of being dropped as "already requested".
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t win = backend.windows.begin()->first;

  sm.set_toplevel_state(surface, ww::WindowState::Fullscreen, true);
  sm.set_wm_state(win, {false, false, true});
  sm.set_wm_state(win, {false, false, false}); // the WM exits fullscreen
  sm.set_toplevel_state(surface, ww::WindowState::Fullscreen, true);

  WW_TEST_CHECK_EQ(backend.window_states.size(), std::size_t{2});
}

WW_TEST_CASE(maximized_on_both_axes_is_reported_as_maximized_and_tiled) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  sm.set_wm_state(win, {/*horz=*/true, /*vert=*/true, false});
  auto states = wl.last_toplevel_states(toplevel);
  WW_TEST_CHECK(has_state(states, XDG_TOPLEVEL_STATE_MAXIMIZED));
  WW_TEST_CHECK(has_state(states, XDG_TOPLEVEL_STATE_TILED_LEFT));

  // One axis is not "maximized" as far as xdg-shell is concerned.
  sm.set_wm_state(win, {/*horz=*/true, /*vert=*/false, false});
  states = wl.last_toplevel_states(toplevel);
  WW_TEST_CHECK(!has_state(states, XDG_TOPLEVEL_STATE_MAXIMIZED));
  WW_TEST_CHECK(!has_state(states, XDG_TOPLEVEL_STATE_TILED_LEFT));
}

WW_TEST_CASE(an_ewmh_update_does_not_erase_the_tiling_i3_reported) {
  // Two sources, two facts: i3's per-edge tiling and the EWMH state. A window
  // manager property update for a tiled window must not clear what i3 said
  // (the old last-call-wins handling did, until i3 happened to speak again).
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  sm.set_tiled_edges(win, ww::TiledEdges{true, true, true, true});
  sm.set_wm_state(win, {false, false, false}); // an unrelated EWMH update
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(toplevel),
                          XDG_TOPLEVEL_STATE_TILED_LEFT));

  // And leaving fullscreen hands the tiling back without i3 re-reporting it.
  sm.set_wm_state(win, {false, false, true});
  WW_TEST_CHECK(!has_state(wl.last_toplevel_states(toplevel),
                           XDG_TOPLEVEL_STATE_TILED_LEFT));
  sm.set_wm_state(win, {false, false, false});
  WW_TEST_CHECK(has_state(wl.last_toplevel_states(toplevel),
                          XDG_TOPLEVEL_STATE_TILED_LEFT));
}

WW_TEST_CASE(tiling_a_window_that_is_not_ours_is_ignored) {
  Harness h;
  // The watcher reports every window the window manager has, not only ours.
  h.sm.set_tiled(0x12345678, true);
  h.sm.set_tiled(0, true);
  WW_TEST_CHECK(true); // reaching here without a crash is the assertion
}

WW_TEST_CASE(set_tiled_edges_sends_exactly_the_edges_given) {
  // set_tiled()'s all-four-together case is covered above; this is its
  // per-edge counterpart, for a window manager that can say which
  // specific edges are part of the tiling arrangement. i3 itself never
  // actually produces a partial combination (see ww::TiledEdges' own
  // comment for why), so this is a direct, synthetic exercise of the
  // plumbing through SurfaceManager rather than anything reachable via
  // I3Watcher today -- the pipeline supports it regardless of whether
  // the one real caller currently varies it.
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  wl_resource *toplevel = wl.xdg_toplevel(2);
  sm.set_toplevel(surface, toplevel, xdg);
  const uint32_t win = h.backend.windows.begin()->first;

  sm.set_tiled_edges(win, ww::TiledEdges{.left = true, .top = true});
  const auto states = wl.last_toplevel_states(toplevel);
  WW_TEST_CHECK(has_state(states, XDG_TOPLEVEL_STATE_TILED_LEFT));
  WW_TEST_CHECK(has_state(states, XDG_TOPLEVEL_STATE_TILED_TOP));
  WW_TEST_CHECK(!has_state(states, XDG_TOPLEVEL_STATE_TILED_RIGHT));
  WW_TEST_CHECK(!has_state(states, XDG_TOPLEVEL_STATE_TILED_BOTTOM));

  // Narrowing to a single edge reconfigures too, not just the
  // tiled/floating transition set_tiled()'s own dedup test covers.
  const auto before = wl.configure_serials(xdg).size();
  sm.set_tiled_edges(win, ww::TiledEdges{.left = true});
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before + 1);
  const auto narrowed = wl.last_toplevel_states(toplevel);
  WW_TEST_CHECK(has_state(narrowed, XDG_TOPLEVEL_STATE_TILED_LEFT));
  WW_TEST_CHECK(!has_state(narrowed, XDG_TOPLEVEL_STATE_TILED_TOP));

  // Repeating the exact same edges is a no-op, same as set_tiled()'s own.
  const auto before2 = wl.configure_serials(xdg).size();
  sm.set_tiled_edges(win, ww::TiledEdges{.left = true});
  WW_TEST_CHECK_EQ(wl.configure_serials(xdg).size(), before2);
}

// --- xdg_toplevel.set_fullscreen --------------------------------------------

WW_TEST_CASE(set_fullscreen_is_forwarded_to_the_window_manager) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t win = backend.windows.begin()->first;

  // The window manager, not this compositor, owns top-level geometry: an
  // ordinary toplevel only ever gets the work area (screen minus panels),
  // and asking for fullscreen is the only route to the whole screen.
  sm.set_toplevel_state(surface, ww::WindowState::Fullscreen, true);

  WW_TEST_CHECK_EQ(backend.window_states.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.window_states.front().window, win);
  WW_TEST_CHECK(backend.window_states.front().state ==
                ww::WindowState::Fullscreen);
  WW_TEST_CHECK(backend.window_states.front().enabled);
}

WW_TEST_CASE(requesting_a_state_the_toplevel_already_has_is_a_no_op) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_toplevel_state(surface, ww::WindowState::Fullscreen, true);
  sm.set_toplevel_state(surface, ww::WindowState::Fullscreen, true);

  // Re-asking would cost a redundant round trip to the WM and a redundant
  // configure to the client, both of which force a redraw.
  WW_TEST_CHECK_EQ(backend.window_states.size(), std::size_t{1});
}

// --- xdg_toplevel.move / .resize / .set_minimized ---------------------------

WW_TEST_CASE(move_toplevel_is_forwarded_to_the_window_manager) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t win = backend.windows.begin()->first;

  sm.move_toplevel(surface);

  WW_TEST_CHECK_EQ(backend.interactive_moves.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.interactive_moves.front(), win);
}

WW_TEST_CASE(resize_toplevel_forwards_the_requested_edge) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t win = backend.windows.begin()->first;

  sm.resize_toplevel(surface, static_cast<uint32_t>(ww::ResizeEdge::BottomLeft));

  WW_TEST_CHECK_EQ(backend.interactive_resizes.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.interactive_resizes.front().window, win);
  WW_TEST_CHECK(backend.interactive_resizes.front().edge ==
                ww::ResizeEdge::BottomLeft);
}

WW_TEST_CASE(resize_toplevel_maps_an_unrecognized_edge_to_none) {
  // Anything a protocol-conforming client can send is one of the nine
  // xdg_toplevel_resize_edge values already covered by
  // resize_toplevel_forwards_the_requested_edge; this exercises what
  // happens to a wire value outside that set (a serial the wire format
  // permits but the protocol never sends), rather than reinterpreting an
  // arbitrary bit pattern as a real edge.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  sm.resize_toplevel(surface, 0xffffu);

  WW_TEST_CHECK_EQ(backend.interactive_resizes.size(), std::size_t{1});
  WW_TEST_CHECK(backend.interactive_resizes.front().edge ==
                ww::ResizeEdge::None);
}

WW_TEST_CASE(minimize_toplevel_is_forwarded_to_the_window_manager) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t win = backend.windows.begin()->first;

  sm.minimize_toplevel(surface);

  WW_TEST_CHECK_EQ(backend.minimized_windows.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.minimized_windows.front(), win);
}

WW_TEST_CASE(move_resize_minimize_are_ignored_before_the_surface_has_a_window) {
  // Mirrors the defensive checks set_toplevel_state already has: a surface
  // that exists but was never given an xdg_toplevel role (or hasn't been
  // through set_toplevel yet) has no X11 window for the backend to act on.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);

  sm.move_toplevel(surface);
  sm.resize_toplevel(surface, static_cast<uint32_t>(ww::ResizeEdge::Top));
  sm.minimize_toplevel(surface);

  WW_TEST_CHECK(backend.interactive_moves.empty());
  WW_TEST_CHECK(backend.interactive_resizes.empty());
  WW_TEST_CHECK(backend.minimized_windows.empty());
}


namespace {
bool g_test_callback_resource_destroyed = false;
void on_test_callback_resource_destroyed(wl_listener *listener, void *) {
  wl_list_remove(&listener->link);
  wl_list_init(&listener->link);
  g_test_callback_resource_destroyed = true;
}
} // namespace

WW_TEST_CASE(explicit_null_attachment_detaches_and_clears_retained_image) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t window = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(64, 64, /*pixmap=*/77));
  sm.commit(surface);
  WW_TEST_CHECK(backend.retained_frames.contains(window));
  WW_TEST_CHECK(backend.windows.at(window).mapped);

  sm.attach_buffer(surface, nullptr);
  sm.commit(surface);

  WW_TEST_CHECK(!backend.retained_frames.contains(window));
  WW_TEST_CHECK(!backend.windows.at(window).mapped);
  WW_TEST_CHECK(backend.called("unmap_window"));
  WW_TEST_CHECK(backend.called("clear_retained_frame"));

  // An X11 Expose after detach must not resurrect the old image.
  sm.on_expose(window);
  WW_TEST_CHECK(backend.called("repaint_window"));
  WW_TEST_CHECK(backend.repainted_windows_with_frame.empty());
}

WW_TEST_CASE(expose_repaints_retained_image_after_buffer_resource_dies) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t window = backend.windows.begin()->first;
  wl_resource *buffer = wl.dmabuf_buffer(64, 64, /*pixmap=*/77);
  sm.attach_buffer(surface, buffer);
  sm.commit(surface);
  WW_TEST_CHECK(backend.retained_frames.contains(window));

  // Destroying the resource clears Surface::last_buffer. The backend's
  // owned presentation image must nevertheless survive for Expose handling.
  wl_resource_destroy(buffer);
  sm.on_expose(window);
  WW_TEST_CHECK(backend.retained_frames.contains(window));
  WW_TEST_CHECK_EQ(backend.repainted_windows_with_frame.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.repainted_windows_with_frame.front(), window);
}

WW_TEST_CASE(late_dmabuf_import_does_not_replace_a_newer_commit) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  backend.defer_import_callbacks = true;

  wl_resource *surface = make_toplevel(sm, wl);
  wl_resource *old_buffer = wl.dmabuf_buffer(64, 64);
  auto *old_dma = static_cast<ww::ImportedBuffer *>(
      wl_resource_get_user_data(old_buffer));
  old_dma->fd = 123;
  old_dma->depth = 24;
  old_dma->bpp = 32;
  old_dma->stride = 64u * 4u;
  sm.attach_buffer(surface, old_buffer);
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.pending_import_callbacks.size(), std::size_t{1});
  WW_TEST_CHECK(backend.presented_pixmaps.empty());

  sm.attach_buffer(surface, wl.dmabuf_buffer(64, 64, /*pixmap=*/88));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_pixmaps.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.presented_pixmaps.back(), uint32_t{88});

  // The older import completes after the newer frame is already current.
  backend.finish_import(0, /*late pixmap=*/777);
  WW_TEST_CHECK_EQ(backend.presented_pixmaps.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(backend.presented_pixmaps.back(), uint32_t{88});
}

WW_TEST_CASE(superseded_dmabuf_import_still_completes_its_frame_callbacks) {
  // A commit whose dmabuf is still being imported gets replaced by a newer
  // commit of a different buffer. The older commit's wl_surface.frame
  // callbacks must still fire (with the newer frame): a client that paces
  // its rendering on them, such as Firefox, never commits again if one is
  // silently dropped, which looks like a frozen window.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  backend.defer_import_callbacks = true;
  g_test_callback_resource_destroyed = false;

  wl_resource *surface = make_toplevel(sm, wl);
  wl_resource *old_buffer = wl.dmabuf_buffer(64, 64);
  auto *old_dma = static_cast<ww::ImportedBuffer *>(
      wl_resource_get_user_data(old_buffer));
  old_dma->fd = 123;
  old_dma->depth = 24;
  old_dma->bpp = 32;
  old_dma->stride = 64u * 4u;

  wl_resource *first_callback = wl.callback();
  wl_listener listener{};
  wl_list_init(&listener.link);
  listener.notify = on_test_callback_resource_destroyed;
  wl_resource_add_destroy_listener(first_callback, &listener);
  sm.add_frame_callback(surface, first_callback);
  sm.attach_buffer(surface, old_buffer);
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.pending_import_callbacks.size(), std::size_t{1});
  WW_TEST_CHECK(!g_test_callback_resource_destroyed);

  const uint32_t window = backend.windows.begin()->first;
  const auto fired = [window] {
    return ww::diag::state()
        .wins[window]
        .n[static_cast<size_t>(ww::diag::Ev::FcbFired)];
  };
  const auto fired_before = fired();

  sm.attach_buffer(surface, wl.dmabuf_buffer(64, 64, /*pixmap=*/88));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_pixmaps.size(), std::size_t{1});
  WW_TEST_CHECK(g_test_callback_resource_destroyed);
  // Destroying the wl_callback is not enough: the client must receive
  // wl_callback.done, or it keeps waiting for it. (Delivered callbacks are
  // counted by the frame-lifecycle diagnostics; a cancelled one is not.)
  WW_TEST_CHECK_EQ(fired(), fired_before + 1);
}

WW_TEST_CASE(frame_callbacks_on_a_surface_without_a_window_still_complete) {
  // A commit that finds no X11 window (no role yet, or a subsurface whose
  // parent has none) is parked. Its wl_surface.frame callbacks must still
  // complete: a client such as Firefox drives its refresh loop from them and
  // will not commit again until one fires, so a parked callback is a frozen
  // window for as long as the surface stays that way.
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;
  g_test_callback_resource_destroyed = false;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *callback = wl.callback();
  wl_listener listener{};
  wl_list_init(&listener.link);
  listener.notify = on_test_callback_resource_destroyed;
  wl_resource_add_destroy_listener(callback, &listener);
  sm.add_frame_callback(surface, callback);
  sm.commit(surface);

  // Paced like a real frame (not an immediate spin), but it does arrive.
  WW_TEST_CHECK(!g_test_callback_resource_destroyed);
  h.timer.advance(std::chrono::milliseconds(20));
  WW_TEST_CHECK(g_test_callback_resource_destroyed);
}

WW_TEST_CASE(surface_teardown_destroys_outstanding_frame_callbacks) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  backend.defer_presented_callbacks = true;
  g_test_callback_resource_destroyed = false;

  wl_resource *surface = make_toplevel(sm, wl);
  wl_resource *callback = wl.callback();
  wl_listener listener{};
  wl_list_init(&listener.link);
  listener.notify = on_test_callback_resource_destroyed;
  wl_resource_add_destroy_listener(callback, &listener);
  sm.add_frame_callback(surface, callback);
  sm.attach_buffer(surface, wl.dmabuf_buffer(64, 64, /*pixmap=*/77));
  sm.commit(surface);

  WW_TEST_CHECK_EQ(backend.pending_presented_callbacks.size(), std::size_t{1});
  sm.destroy_surface(surface);
  WW_TEST_CHECK(g_test_callback_resource_destroyed);

  // A completion arriving after teardown is harmless and cannot use the
  // already-destroyed wl_callback resource.
  backend.finish_next_presentation();
  WW_TEST_CHECK(g_test_callback_resource_destroyed);
}

WW_TEST_CASE(configure_acknowledgements_accept_sent_serials_only) {
  Harness h;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg_surface = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg_surface);
  uint32_t window = 0;
  for (const auto &[id, _] : h.backend.windows) {
    window = id;
    break;
  }
  sm.request_resize(window, 900, 700);
  sm.request_resize(window, 901, 700);
  const auto serials = wl.configure_serials(xdg_surface);
  WW_TEST_CHECK(serials.size() >= std::size_t{3});
  if (serials.size() >= 3) {
    WW_TEST_CHECK(sm.acknowledge_configure(surface, serials.back()));
    // Acknowledging the newest configure consumes that serial and all older
    // ones; a duplicate or older ack is not considered valid thereafter.
    WW_TEST_CHECK(!sm.acknowledge_configure(surface, serials.front()));
    WW_TEST_CHECK(!sm.acknowledge_configure(surface, serials.back()));
  }
  WW_TEST_CHECK(!sm.acknowledge_configure(surface, 0xfedcba98u));
}


WW_TEST_CASE(destroyed_pending_buffer_resource_is_not_a_null_detach) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *surface = make_toplevel(sm, wl);
  const uint32_t window = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(64, 64, /*pixmap=*/77));
  sm.commit(surface);
  WW_TEST_CHECK(backend.retained_frames.contains(window));

  wl_resource *pending = wl.dmabuf_buffer(80, 80, /*pixmap=*/88);
  sm.attach_buffer(surface, pending);
  wl_resource_destroy(pending);
  sm.commit(surface);

  // The pending resource disappearing invalidates that attachment; only an
  // explicit attach(nullptr) is an instruction to unmap the committed image.
  WW_TEST_CHECK(backend.retained_frames.contains(window));
  WW_TEST_CHECK(backend.windows.at(window).mapped);
  WW_TEST_CHECK(!backend.called("unmap_window"));
}


WW_TEST_CASE(dmabuf_release_waits_for_source_snapshot_fence) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  backend.defer_source_consumed_callbacks = true;

  wl_resource *surface = make_toplevel(sm, wl);
  wl_resource *buffer = wl.dmabuf_buffer(64, 64, /*pixmap=*/77);
  auto *imported = static_cast<ww::ImportedBuffer *>(
      wl_resource_get_user_data(buffer));
  sm.attach_buffer(surface, buffer);
  sm.commit(surface);

  // The RGB pixmap presentation queued a compositor-owned snapshot. The
  // client must not receive release until the fake backend says the X-server
  // copy fence has passed, because it could otherwise overwrite the source
  // while that copy is still being read.
  WW_TEST_CHECK_EQ(imported->use_state->source_reads_in_flight, uint32_t{1});
  WW_TEST_CHECK(imported->use_state->release_pending);
  WW_TEST_CHECK_EQ(wl.count_events(buffer, WL_BUFFER_RELEASE), std::size_t{0});

  backend.finish_next_source_consumed();
  WW_TEST_CHECK_EQ(imported->use_state->source_reads_in_flight, uint32_t{0});
  WW_TEST_CHECK(!imported->use_state->release_pending);
  WW_TEST_CHECK_EQ(wl.count_events(buffer, WL_BUFFER_RELEASE), std::size_t{1});
}

// --- review follow-ups: subtree teardown, recycled sibling, WM fights --------

WW_TEST_CASE(destroying_an_ancestor_forgets_the_whole_subsurface_tree) {
  // X11 destroys the entire descendant tree with the ancestor window, so the
  // bookkeeping of grandchildren must be dropped too; otherwise destroying a
  // grandchild later issues destroy_window() on an id X already recycled, and
  // surface_for_window() keeps routing events for a dead id.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *a = make_toplevel(sm, wl);
  wl_resource *b = wl.surface();
  sm.create_surface(b);
  sm.create_subsurface(wl.subsurface(), b, a);
  const uint32_t b_win = backend.next_id - 1;
  wl_resource *c = wl.surface();
  sm.create_surface(c);
  sm.create_subsurface(wl.subsurface(), c, b);
  const uint32_t c_win = backend.next_id - 1;
  WW_TEST_CHECK(c_win != b_win);
  WW_TEST_CHECK_EQ(backend.windows.at(c_win).parent, b_win);

  sm.destroy_surface(a);
  WW_TEST_CHECK(sm.surface_for_window(c_win) == nullptr);   // no stale routing
  sm.destroy_surface(c);
  WW_TEST_CHECK(backend.destroyed.count(c_win) == 0);       // no double destroy
  WW_TEST_CHECK(backend.forgotten.count(c_win) == 1);
}

WW_TEST_CASE(a_destroyed_siblings_pending_restack_is_not_resolved_against_a_recycled_resource) {
  // pending_restack_sibling is a raw wl_resource*. If the sibling dies and a
  // new surface is created at the same address before the parent applies, the
  // restack must not silently target the unrelated new surface.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;

  wl_resource *parent = make_toplevel(sm, wl);
  wl_resource *sibling = wl.surface();
  sm.create_surface(sibling);
  sm.create_subsurface(wl.subsurface(), sibling, parent);
  wl_resource *child = wl.surface();
  sm.create_surface(child);
  sm.create_subsurface(wl.subsurface(), child, parent);
  const uint32_t child_win = backend.next_id - 1;

  sm.place_subsurface(child, sibling, /*above=*/true);
  sm.destroy_surface(sibling);

  // Simulate allocator reuse: an unrelated surface appears at the same address.
  sm.create_surface(sibling);
  sm.create_subsurface(wl.subsurface(), sibling, parent);
  const uint32_t recycled_win = backend.next_id - 1;
  WW_TEST_CHECK(recycled_win != child_win);

  backend.restacks.clear();
  sm.commit(parent);
  bool restacked_child = false;
  for (const auto &r : backend.restacks) {
    if (r.window_id != child_win)
      continue;
    restacked_child = true;
    WW_TEST_CHECK(r.sibling_id != recycled_win);  // must not hit the new surface
  }
  WW_TEST_CHECK(restacked_child);  // the request itself is still honoured
}

WW_TEST_CASE(wm_reasserting_its_target_after_adoption_does_not_re_adopt_in_a_loop) {
  // A size-enforcing WM (tiling) that puts the window back to its target after
  // we adopted the client's smaller committed size must be left alone for that
  // target; otherwise adopt -> WM re-tiles -> adopt ... spins with no delay.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(320, 240, /*pixmap=*/301));
  sm.commit(surface);

  sm.request_resize(win, 1920, 1080);
  const uint32_t serial = wl.configure_serials(xdg).back();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, serial));
  h.timer.advance(std::chrono::milliseconds{250});
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{320});  // adopted once
  const std::size_t after_first_adoption = backend.count("resize_window");

  for (int i = 0; i < 5; ++i) {
    sm.request_resize(win, 1920, 1080);          // WM puts it back
    h.timer.advance(std::chrono::milliseconds{0});
  }
  WW_TEST_CHECK_EQ(backend.count("resize_window"), after_first_adoption);

  // A genuinely new WM target (user resizes again) is evaluated afresh.
  sm.request_resize(win, 1000, 700);
  const uint32_t serial2 = wl.configure_serials(xdg).back();
  WW_TEST_CHECK(sm.acknowledge_configure(surface, serial2));
  h.timer.advance(std::chrono::milliseconds{250});
  WW_TEST_CHECK_EQ(backend.count("resize_window"), after_first_adoption + 1);
}

WW_TEST_CASE(wm_owned_geometry_commit_at_the_wm_target_is_presented_without_stretching) {
  // Fullscreen/maximized: a frame that already has the WM's size is not stale
  // and must not be pushed through the stretch path.
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl_resource *surface = wl.surface();
  sm.create_surface(surface);
  wl_resource *xdg = wl.xdg_surface();
  sm.set_toplevel(surface, wl.xdg_toplevel(), xdg);
  const uint32_t win = backend.windows.begin()->first;
  sm.attach_buffer(surface, wl.dmabuf_buffer(640, 480, /*pixmap=*/311));
  sm.commit(surface);

  sm.set_wm_state(win, {/*horz=*/false, /*vert=*/false, /*fullscreen=*/true});
  sm.request_resize(win, 1024, 768);
  WW_TEST_CHECK(sm.acknowledge_configure(surface, wl.configure_serials(xdg).back()));
  backend.clear_calls();
  sm.attach_buffer(surface, wl.dmabuf_buffer(1024, 768, /*pixmap=*/312));
  sm.commit(surface);
  WW_TEST_CHECK(!backend.called("present_pixmap_scaled"));
  WW_TEST_CHECK(!backend.called("repaint_window_scaled"));
}

// --- wp_viewport (viewporter) -------------------------------------------------

WW_TEST_CASE(viewport_source_rectangle_crops_buffer) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // The viewport resource is created but we use the public API directly
  // (wl.make(&wp_viewport_interface, 1) would create it but we don't need to hold it)
  (void)wl.make(&wp_viewport_interface, 1);
  const wl_fixed_t src_x = wl_fixed_from_int(100);
  const wl_fixed_t src_y = wl_fixed_from_int(50);
  const wl_fixed_t src_w = wl_fixed_from_int(600);
  const wl_fixed_t src_h = wl_fixed_from_int(500);
  sm.set_pending_viewport_source(surface, src_x, src_y, src_w, src_h);
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/401));
  sm.commit(surface);

  // The presented region should be the source rectangle
  const auto &r = backend.presented_regions.back();
  WW_TEST_CHECK_EQ(r.x, int16_t{100});
  WW_TEST_CHECK_EQ(r.y, int16_t{50});
  WW_TEST_CHECK_EQ(r.width, uint16_t{600});
  WW_TEST_CHECK_EQ(r.height, uint16_t{500});
}

WW_TEST_CASE(viewport_destination_size_scales_surface) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // Set destination size to 400x300 (half the buffer)
  sm.set_pending_viewport_destination(surface, 400, 300);
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/402));
  sm.commit(surface);

  // The surface/window size should become the destination size
  const uint32_t win = backend.windows.begin()->first;
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{400});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{300});
  // The presented region should be the full buffer (no source crop)
  const auto &r = backend.presented_regions.back();
  WW_TEST_CHECK_EQ(r.width, uint16_t{800});
  WW_TEST_CHECK_EQ(r.height, uint16_t{600});
}

WW_TEST_CASE(viewport_source_and_destination_combined) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // Crop to 400x300 at (200,150) and scale to 200x150
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_int(200),
                                 wl_fixed_from_int(150),
                                 wl_fixed_from_int(400),
                                 wl_fixed_from_int(300));
  sm.set_pending_viewport_destination(surface, 200, 150);
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/403));
  sm.commit(surface);

  // The surface/window size should become the destination size
  const uint32_t win = backend.windows.begin()->first;
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{200});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{150});
  // The presented region should be the source rectangle
  const auto &r = backend.presented_regions.back();
  WW_TEST_CHECK_EQ(r.x, int16_t{200});
  WW_TEST_CHECK_EQ(r.y, int16_t{150});
  WW_TEST_CHECK_EQ(r.width, uint16_t{400});
  WW_TEST_CHECK_EQ(r.height, uint16_t{300});
}

WW_TEST_CASE(viewport_unset_restores_full_buffer) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // First commit with viewport
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(600),
                                 wl_fixed_from_int(400));
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/404));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_regions.back().width, uint16_t{600});

  // Second commit with viewport unset (all -1)
  const wl_fixed_t minus_one = wl_fixed_from_int(-1);
  sm.set_pending_viewport_source(surface, minus_one, minus_one, minus_one, minus_one);
  sm.set_pending_viewport_destination(surface, -1, -1);
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/405));
  sm.commit(surface);

  // Should restore full buffer
  const auto &r = backend.presented_regions.back();
  WW_TEST_CHECK_EQ(r.x, int16_t{0});
  WW_TEST_CHECK_EQ(r.y, int16_t{0});
  WW_TEST_CHECK_EQ(r.width, uint16_t{800});
  WW_TEST_CHECK_EQ(r.height, uint16_t{600});
}

WW_TEST_CASE(viewport_destruction_removes_state) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // Set viewport
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(600),
                                 wl_fixed_from_int(400));
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/406));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_regions.back().width, uint16_t{600});

  // Destroy the viewport by unsetting (simulating viewport destroy)
  // The viewport destroy handler unsets the state on next commit
  const wl_fixed_t minus_one = wl_fixed_from_int(-1);
  sm.set_pending_viewport_source(surface, minus_one, minus_one, minus_one, minus_one);
  sm.set_pending_viewport_destination(surface, -1, -1);
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/407));
  sm.commit(surface);

  // Should restore full buffer
  const auto &r = backend.presented_regions.back();
  WW_TEST_CHECK_EQ(r.width, uint16_t{800});
  WW_TEST_CHECK_EQ(r.height, uint16_t{600});
}

WW_TEST_CASE(viewport_fractional_source_coordinates) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // Use fractional coordinates (wl_fixed_t)
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_double(100.5),
                                 wl_fixed_from_double(50.25),
                                 wl_fixed_from_double(600.75),
                                 wl_fixed_from_double(500.5));
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/408));
  sm.commit(surface);

  const auto &r = backend.presented_regions.back();
  // Coordinates should be rounded/truncated to integers for presentation
  WW_TEST_CHECK_EQ(r.x, int16_t{100});  // truncated
  WW_TEST_CHECK_EQ(r.y, int16_t{50});   // truncated
  // Width/height should also be truncated
  WW_TEST_CHECK_EQ(r.width, uint16_t{600});
  WW_TEST_CHECK_EQ(r.height, uint16_t{500});
}

WW_TEST_CASE(viewport_with_buffer_scale) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // Set buffer_scale to 2 (buffer is 1600x1200 for 800x600 surface)
  sm.set_pending_buffer_scale(surface, 2);
  // Viewport source is in surface-local coordinates (after buffer_scale)
  // Surface is 800x600, so source (100,50,600,500) in surface coords
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(50),
                                 wl_fixed_from_int(600),
                                 wl_fixed_from_int(500));
  sm.attach_buffer(surface, wl.dmabuf_buffer(1600, 1200, /*pixmap=*/409));
  sm.commit(surface);

  const auto &r = backend.presented_regions.back();
  // Presented region is in buffer pixels: source * buffer_scale
  WW_TEST_CHECK_EQ(r.x, int16_t{200});   // 100 * 2
  WW_TEST_CHECK_EQ(r.y, int16_t{100});   // 50 * 2
  WW_TEST_CHECK_EQ(r.width, uint16_t{1200}); // 600 * 2
  WW_TEST_CHECK_EQ(r.height, uint16_t{1000}); // 500 * 2
}

WW_TEST_CASE(viewport_with_offset) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 10, 10, 800, 600);
  // Set viewport source
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(50),
                                 wl_fixed_from_int(600),
                                 wl_fixed_from_int(500));
  // Set offset - shifts the buffer relative to surface
  // offset (-20, -10) means buffer origin is at surface (20, 10)
  sm.set_pending_offset(surface, -20, -10);
  sm.attach_buffer(surface, wl.dmabuf_buffer(820, 610, /*pixmap=*/410));
  sm.commit(surface);

  const auto &r = backend.presented_regions.back();
  // Presented region is in BUFFER coordinates:
  // Offset (-20, -10): surface (0,0) = buffer (20, 10)
  // Viewport source at surface (100, 50) = buffer (120, 60)
  WW_TEST_CHECK_EQ(r.x, int16_t{120});
  WW_TEST_CHECK_EQ(r.y, int16_t{60});
  WW_TEST_CHECK_EQ(r.width, uint16_t{600});
  WW_TEST_CHECK_EQ(r.height, uint16_t{500});
}

WW_TEST_CASE(viewport_only_destination_affects_surface_size) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // Only set destination - no source rectangle
  sm.set_pending_viewport_destination(surface, 400, 300);
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/411));
  sm.commit(surface);

  // Surface/window size becomes destination size, full buffer is scaled
  const uint32_t win = backend.windows.begin()->first;
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{400});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{300});
  // Presented region is the full buffer (no source crop)
  const auto &r = backend.presented_regions.back();
  WW_TEST_CHECK_EQ(r.x, int16_t{0});
  WW_TEST_CHECK_EQ(r.y, int16_t{0});
  WW_TEST_CHECK_EQ(r.width, uint16_t{800});
  WW_TEST_CHECK_EQ(r.height, uint16_t{600});
}

WW_TEST_CASE(viewport_only_source_crops_without_scaling) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // Only set source - no destination size
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_int(200),
                                 wl_fixed_from_int(150),
                                 wl_fixed_from_int(400),
                                 wl_fixed_from_int(300));
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/412));
  sm.commit(surface);

  const auto &r = backend.presented_regions.back();
  // Surface size becomes source rectangle size (cropping without scaling)
  WW_TEST_CHECK_EQ(r.x, int16_t{200});
  WW_TEST_CHECK_EQ(r.y, int16_t{150});
  WW_TEST_CHECK_EQ(r.width, uint16_t{400});
  WW_TEST_CHECK_EQ(r.height, uint16_t{300});
}

WW_TEST_CASE(viewport_multiple_commits_state_persists) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(600),
                                 wl_fixed_from_int(400));
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/415));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_regions.back().width, uint16_t{600});

  // Second commit without changing viewport - state should persist
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/416));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_regions.back().width, uint16_t{600});

  // Third commit with new buffer - viewport still applies
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/417));
  sm.commit(surface);
  WW_TEST_CHECK_EQ(backend.presented_regions.back().width, uint16_t{600});
}

WW_TEST_CASE(viewport_double_buffering_source_destination) {
  Harness h;
  auto &backend = h.backend;
  auto &wl = h.wl;
  auto &sm = h.sm;
  wl.register_viewporter();

  wl_resource *surface = make_toplevel(sm, wl);
  sm.set_window_geometry(surface, 0, 0, 800, 600);
  // Set source and destination
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(100),
                                 wl_fixed_from_int(600),
                                 wl_fixed_from_int(400));
  sm.set_pending_viewport_destination(surface, 300, 200);
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/418));
  sm.commit(surface);

  // Window size should be destination size
  const uint32_t win = backend.windows.begin()->first;
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{300});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{200});
  // Presented region should be source rectangle
  const auto &r1 = backend.presented_regions.back();
  WW_TEST_CHECK_EQ(r1.x, int16_t{100});
  WW_TEST_CHECK_EQ(r1.y, int16_t{100});
  WW_TEST_CHECK_EQ(r1.width, uint16_t{600});
  WW_TEST_CHECK_EQ(r1.height, uint16_t{400});

  // Change only source on next commit
  sm.set_pending_viewport_source(surface,
                                 wl_fixed_from_int(200),
                                 wl_fixed_from_int(200),
                                 wl_fixed_from_int(400),
                                 wl_fixed_from_int(200));
  sm.attach_buffer(surface, wl.dmabuf_buffer(800, 600, /*pixmap=*/419));
  sm.commit(surface);

  // Window size should still be destination size (unchanged)
  WW_TEST_CHECK_EQ(backend.windows.at(win).width, uint16_t{300});
  WW_TEST_CHECK_EQ(backend.windows.at(win).height, uint16_t{200});
  // Presented region should be new source rectangle
  const auto &r2 = backend.presented_regions.back();
  WW_TEST_CHECK_EQ(r2.x, int16_t{200});
  WW_TEST_CHECK_EQ(r2.y, int16_t{200});
  WW_TEST_CHECK_EQ(r2.width, uint16_t{400});
  WW_TEST_CHECK_EQ(r2.height, uint16_t{200});
}
