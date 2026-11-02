// End-to-end coverage for xdg_toplevel.set_parent -> WM_TRANSIENT_FOR: a
// real libwayland client, talking to a real SurfaceManager, talking to a
// real X11Backend against a real (throwaway) X server. Every other
// SurfaceManager suite substitutes FakeWindowBackend for the X11 half
// specifically to avoid this -- this is the one place that doesn't, because
// what is under test here is that a chain of three real layers agrees, not
// any single one of them in isolation.
//
// Deliberately does not need a window manager: WM_TRANSIENT_FOR is a
// property X11Backend writes directly onto the window it created (see
// X11Backend::set_window_transient_for), and both windows stay direct
// children of the root window with nothing running to reparent them. An
// independent second xcb connection (Inspector, shared with
// x11_backend_tests.cpp and dri3_tests.cpp) reads it back, the same
// separation of "what we told the server" from "our own cache of what we
// told the server" every suite in this directory insists on.

#include "../fakes/wayland_client_fixture.hpp"
#include "x11_harness.hpp"

#include <wwindows/adapters/wayland/globals/core_globals.hpp>
#include <wwindows/adapters/wayland/globals/xdg_shell.hpp>
#include <wwindows/adapters/wayland/protocols/xdg-shell-client-protocol.h>
#include <wwindows/core/surface_manager.hpp>
#include <wwindows/input/input_translator.hpp>

#include "../support/test.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using ww_test::Inspector;
using ww_test::WaylandClientFixture;

namespace {

constexpr uint32_t kOutputWidth = 1920;
constexpr uint32_t kOutputHeight = 1080;
constexpr uint32_t kOutputMmWidth = 520;
constexpr uint32_t kOutputMmHeight = 320;

// Real X11Backend + real SurfaceManager/InputTranslator + a real libwayland
// client over a real socket, all three at once -- see the file header for
// why this suite exists separately from every other one that substitutes a
// fake for the X11 half.
struct Compositor {
  std::unique_ptr<ww::EventLoop> loop;
  std::unique_ptr<ww::X11Backend> backend;
  std::unique_ptr<ww::SurfaceManager> manager;
  std::unique_ptr<ww::InputTranslator> input;
  WaylandClientFixture wl;
  Inspector inspector;

  Compositor() {
    ww_test::keep_server_alive();

    auto l = ww::EventLoop::create();
    if (!l)
      throw std::runtime_error("EventLoop::create failed");
    loop = std::move(*l);

    auto b = ww::X11Backend::create(*loop);
    if (!b)
      throw std::runtime_error("X11Backend::create failed: " +
                               b.error().message);
    backend = std::move(*b);

    manager = std::make_unique<ww::SurfaceManager>(*backend);
    input = std::make_unique<ww::InputTranslator>(*backend, *manager);
    input->set_display(wl.server_display());

    const ww::OutputState output{kOutputWidth, kOutputHeight, kOutputMmWidth,
                                 kOutputMmHeight};
    if (!ww::register_core_globals(wl.server_display(), *manager, *input,
                                   output))
      throw std::runtime_error("register_core_globals failed");
    if (!ww::register_xdg_shell(wl.server_display(), *manager))
      throw std::runtime_error("register_xdg_shell failed");
    wl.pump();
  }

  // Force every queued xcb request out and wait for the server to catch
  // up, so the Inspector observes a settled state rather than racing
  // SurfaceManager's own calls into the backend. Same technique as
  // x11_harness.hpp's own Harness::sync(), duplicated rather than shared
  // because that one is a method on a type this file doesn't construct.
  void x11_sync() {
    xcb_flush(backend->connection());
    auto *reply = xcb_get_input_focus_reply(
        backend->connection(), xcb_get_input_focus(backend->connection()),
        nullptr);
    ::free(reply);
  }
};

// Creates a toplevel, sets app_id (WM_CLASS) to `app_id` so the Inspector
// can identify which real X11 window it became without any test-only
// accessor into SurfaceManager's private surface map -- everything this
// suite checks, it checks the same way an outside observer (a real window
// manager, `xprop`) would.
//
// Returns every client-side proxy it created: destroying only the toplevel
// would leak the xdg_surface and wl_surface proxies (LeakSanitizer).
struct ClientToplevel {
  wl_surface *surface;
  xdg_surface *xdg;
  xdg_toplevel *toplevel;
  void destroy() {
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg);
    wl_surface_destroy(surface);
  }
};

ClientToplevel make_identifiable_toplevel(Compositor &c, wl_compositor *comp,
                                          xdg_wm_base *base,
                                          const char *app_id) {
  wl_surface *surface = wl_compositor_create_surface(comp);
  xdg_surface *xdg = xdg_wm_base_get_xdg_surface(base, surface);
  xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg);
  xdg_toplevel_set_app_id(toplevel, app_id);
  wl_surface_commit(surface);
  c.wl.pump();
  return {surface, xdg, toplevel};
}

// Finds the one child of root whose WM_CLASS contains `app_id`. WM_CLASS is
// (instance, class), both set to the same app_id string by
// SurfaceManager::set_app_id -- either copy is enough to match on.
xcb_window_t find_window_by_app_id(Compositor &c, const std::string &app_id) {
  const xcb_window_t root =
      xcb_setup_roots_iterator(xcb_get_setup(c.inspector.conn())).data->root;
  for (xcb_window_t w : c.inspector.children_of(root)) {
    const std::string wm_class =
        c.inspector.string_property(w, XCB_ATOM_WM_CLASS);
    if (wm_class.find(app_id) != std::string::npos)
      return w;
  }
  return 0;
}

} // namespace

WW_TEST_CASE(setting_a_parent_writes_wm_transient_for_on_the_real_window) {
  Compositor c;
  auto *comp = static_cast<wl_compositor *>(
      c.wl.bind("wl_compositor", &wl_compositor_interface, 4));
  auto *base = static_cast<xdg_wm_base *>(
      c.wl.bind("xdg_wm_base", &xdg_wm_base_interface, 1));

  ClientToplevel parent = make_identifiable_toplevel(
      c, comp, base, "wwindows-e2e-transient-parent");
  ClientToplevel child = make_identifiable_toplevel(
      c, comp, base, "wwindows-e2e-transient-child");

  xdg_toplevel_set_parent(child.toplevel, parent.toplevel);
  c.wl.pump();
  c.x11_sync();

  const xcb_window_t parent_x11 =
      find_window_by_app_id(c, "wwindows-e2e-transient-parent");
  const xcb_window_t child_x11 =
      find_window_by_app_id(c, "wwindows-e2e-transient-child");
  WW_TEST_CHECK(parent_x11 != 0);
  WW_TEST_CHECK(child_x11 != 0);

  const std::vector<uint32_t> transient_for = c.inspector.u32_property(
      child_x11, c.inspector.atom("WM_TRANSIENT_FOR"));
  WW_TEST_CHECK_EQ(transient_for.size(), std::size_t{1});
  if (!transient_for.empty())
    WW_TEST_CHECK_EQ(transient_for.front(),
                     static_cast<uint32_t>(parent_x11));

  // The parent itself must not have picked one up from any of this.
  WW_TEST_CHECK(c.inspector
                    .u32_property(parent_x11,
                                  c.inspector.atom("WM_TRANSIENT_FOR"))
                    .empty());

  child.destroy();
  parent.destroy();
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(clearing_the_parent_removes_wm_transient_for_from_the_real_window) {
  Compositor c;
  auto *comp = static_cast<wl_compositor *>(
      c.wl.bind("wl_compositor", &wl_compositor_interface, 4));
  auto *base = static_cast<xdg_wm_base *>(
      c.wl.bind("xdg_wm_base", &xdg_wm_base_interface, 1));

  ClientToplevel parent = make_identifiable_toplevel(
      c, comp, base, "wwindows-e2e-transient-clear-parent");
  ClientToplevel child = make_identifiable_toplevel(
      c, comp, base, "wwindows-e2e-transient-clear-child");

  xdg_toplevel_set_parent(child.toplevel, parent.toplevel);
  c.wl.pump();
  c.x11_sync();
  const xcb_window_t child_x11 =
      find_window_by_app_id(c, "wwindows-e2e-transient-clear-child");
  WW_TEST_CHECK(child_x11 != 0);
  WW_TEST_CHECK_EQ(
      c.inspector.u32_property(child_x11, c.inspector.atom("WM_TRANSIENT_FOR"))
          .size(),
      std::size_t{1});

  // nullptr makes the toplevel independent again -- see set_toplevel_parent's
  // own doc comment.
  xdg_toplevel_set_parent(child.toplevel, nullptr);
  c.wl.pump();
  c.x11_sync();

  WW_TEST_CHECK(
      c.inspector.u32_property(child_x11, c.inspector.atom("WM_TRANSIENT_FOR"))
          .empty());

  child.destroy();
  parent.destroy();
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}
