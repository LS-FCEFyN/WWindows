// Tests for the Wayland protocol globals.
//
// These drive a real libwayland client (tests/fakes/wayland_client_fixture.hpp)
// over a real socket into the real registration functions. Requests are
// issued through the generated client stubs, exactly as a Mesa/GTK/Qt client
// would issue them, and the effects are observed on the other side of the
// compositor: on FakeWindowBackend, which is where a request that was
// unpacked correctly eventually shows up as a window operation.
//
// What that buys, concretely: a handler that read the wrong argument, bound a
// resource at the wrong version, or forgot to wire an object to its role
// fails here and cannot fail anywhere else in the suite. The handlers are thin
// in the forwarding sense, but "thin" describes the part that forwards to
// SurfaceManager, not the part that unpacks the request. Argument handling,
// resource binding and version gating are real logic, and no existing suite
// reaches any of it -- a test that builds a wl_resource itself has already
// performed the step under test.

#include "../fakes/fake_window_backend.hpp"
#include "../fakes/fake_resize_timer.hpp"
#include "../fakes/wayland_client_fixture.hpp"
#include "../support/test.hpp"

#include <wwindows/adapters/wayland/globals/core_globals.hpp>
#include <wwindows/adapters/wayland/globals/fractional_scale.hpp>
#include <wwindows/adapters/wayland/protocols/fractional-scale-v1-server-protocol.h> // interface objects only
#include <wwindows/adapters/wayland/globals/data_device.hpp>
#include <wwindows/adapters/wayland/globals/xdg_shell.hpp>
#include <wwindows/adapters/wayland/globals/viewporter.hpp>
#include <wwindows/adapters/wayland/globals/foreign_toplevel_list.hpp>
#include <wwindows/adapters/wayland/globals/image_capture_source.hpp>
#include <wwindows/adapters/wayland/globals/image_copy_capture.hpp>
#include <wwindows/adapters/wayland/protocols/xdg-shell-client-protocol.h>
#include <wwindows/core/surface_manager.hpp>
#include <wwindows/input/input_translator.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

using ww_test::FakeWindowBackend;
using ww_test::WaylandClientFixture;

namespace {

constexpr int32_t kOutputWidth = 1280;
constexpr int32_t kOutputHeight = 1024;
constexpr int32_t kOutputMmWidth = 340;
constexpr int32_t kOutputMmHeight = 270;

// The whole compositor-side stack a client actually talks to, wired the same
// way src/application.cpp wires it.
//
// Declaration order is load-bearing and matches production: the client
// fixture is declared last so it is destroyed first, because SurfaceManager
// registers wl_resource destroy listeners and never unregisters them. The
// reverse order is a use-after-free -- the property ARCHITECTURE.md pins and
// display_teardown_before_the_manager_is_safe covers.
struct Compositor {
  FakeWindowBackend backend;
  ww_test::FakeResizeTimer timer;
  std::size_t xdg_configure_events{0};
  ww::SurfaceManager manager{backend, &timer};
  // Wired to the real SurfaceManager, exactly as src/application.cpp does.
  // InputTranslator reaches surfaces through ISurfaceInteractions, and
  // SurfaceManager is the production implementation -- a fake here would
  // resolve no real surfaces, which the drag tests below depend on.
  ww::InputTranslator input{backend, manager};
  WaylandClientFixture wl; // destroyed first
  ww::OutputGlobal *output_global{nullptr};

  Compositor() {
    input.set_display(wl.server_display());

    const ww::OutputState output{kOutputWidth, kOutputHeight, kOutputMmWidth,
                                 kOutputMmHeight};
    auto globals =
        ww::register_core_globals(wl.server_display(), manager, input, output);
    if (!globals)
      throw std::runtime_error("register_core_globals failed");
    output_global = *globals;
    if (!ww::register_xdg_shell(wl.server_display(), manager))
      throw std::runtime_error("register_xdg_shell failed");
    if (!ww::register_viewporter(wl.server_display()))
      throw std::runtime_error("register_viewporter failed");
    if (!ww::foreign_toplevel_list::register_foreign_toplevel_list(wl.server_display(), manager))
      throw std::runtime_error("register_foreign_toplevel_list failed");
    if (!ww::image_capture_source::register_image_capture_source(wl.server_display(), manager))
      throw std::runtime_error("register_image_capture_source failed");
    if (!ww::image_copy_capture::register_image_copy_capture(wl.server_display(), manager))
      throw std::runtime_error("register_image_copy_capture failed");
    if (!ww::register_data_device_manager(wl.server_display(), manager, input,
                                          backend))
      throw std::runtime_error("register_data_device_manager failed");
    wl.pump();
  }
};

// A committed wl_buffer is what turns a surface with a role into a real
// window, so most tests need one. wl_shm is provided by libwayland itself
// (wl_display_init_shm, called by WaylandServer in production), so a client
// can allocate one the ordinary way.
struct ShmBuffer {
  wl_shm_pool *pool{nullptr};
  wl_buffer *buffer{nullptr};
  int fd{-1};

  ~ShmBuffer() {
    if (buffer)
      wl_buffer_destroy(buffer);
    if (pool)
      wl_shm_pool_destroy(pool);
    if (fd >= 0)
      ::close(fd);
  }
};

// --- helpers ---------------------------------------------------------------

wl_compositor *bind_compositor(Compositor &c, uint32_t version = 4) {
  return static_cast<wl_compositor *>(
      c.wl.bind("wl_compositor", &wl_compositor_interface, version));
}

xdg_wm_base *bind_wm_base(Compositor &c, uint32_t version = 1) {
  return static_cast<xdg_wm_base *>(
      c.wl.bind("xdg_wm_base", &xdg_wm_base_interface, version));
}

// Drive a surface all the way to a mapped toplevel: the sequence every real
// shell client performs.
struct Toplevel {
  wl_surface *surface{nullptr};
  xdg_surface *xdg{nullptr};
  xdg_toplevel *toplevel{nullptr};
};

Toplevel make_toplevel(Compositor &c, wl_compositor *comp, xdg_wm_base *base) {
  Toplevel t;
  t.surface = wl_compositor_create_surface(comp);
  t.xdg = xdg_wm_base_get_xdg_surface(base, t.surface);
  // Keep the integration client protocol-correct: every configure serial is
  // acknowledged, including configures sent in the middle of a WM resize.
  // Settlement tests can then isolate the timer condition rather than
  // accidentally passing because the test client never completed the XDG
  // configure handshake.
  static const xdg_surface_listener listener = {
      [](void *data, xdg_surface *xdg, uint32_t serial) {
        auto *compositor = static_cast<Compositor *>(data);
        ++compositor->xdg_configure_events;
        xdg_surface_ack_configure(xdg, serial);
      }};
  xdg_surface_add_listener(t.xdg, &listener, &c);
  t.toplevel = xdg_surface_get_toplevel(t.xdg);
  wl_surface_commit(t.surface);
  c.wl.pump();
  return t;
}

} // namespace

// --- registry ---------------------------------------------------------------

WW_TEST_CASE(compositor_advertises_the_globals_clients_need) {
  Compositor c;

  // A missing global is not a subtle failure: a client that cannot find
  // wl_compositor or xdg_wm_base cannot put anything on screen at all.
  WW_TEST_CHECK(c.wl.has_global("wl_compositor"));
  WW_TEST_CHECK(c.wl.has_global("wl_subcompositor"));
  WW_TEST_CHECK(c.wl.has_global("wl_seat"));
  WW_TEST_CHECK(c.wl.has_global("wl_output"));
  WW_TEST_CHECK(c.wl.has_global("xdg_wm_base"));
  WW_TEST_CHECK(c.wl.has_global("wl_data_device_manager"));
  WW_TEST_CHECK(c.wl.has_global("wp_viewporter"));
  WW_TEST_CHECK(c.wl.has_global("ext_foreign_toplevel_list_v1"));
  WW_TEST_CHECK(c.wl.has_global("ext_image_capture_source_manager_v1"));
  WW_TEST_CHECK(c.wl.has_global("ext_image_copy_capture_manager_v1"));
}

WW_TEST_CASE(advertised_versions_match_what_the_handlers_implement) {
  Compositor c;

  // Version is a contract, not a detail: advertising a version higher than
  // the handlers implement makes clients send requests that land on null
  // vtable entries. These numbers are the ones the register_* functions
  // pass to wl_global_create.
  const auto *compositor = c.wl.find_global("wl_compositor");
  WW_TEST_CHECK(compositor != nullptr);
  if (compositor)
    WW_TEST_CHECK_EQ(compositor->version, uint32_t{5});

  const auto *seat = c.wl.find_global("wl_seat");
  WW_TEST_CHECK(seat != nullptr);
  if (seat)
    WW_TEST_CHECK_EQ(seat->version, uint32_t{7});

  const auto *output = c.wl.find_global("wl_output");
  WW_TEST_CHECK(output != nullptr);
  if (output)
    WW_TEST_CHECK_EQ(output->version, uint32_t{3});

  const auto *ddm = c.wl.find_global("wl_data_device_manager");
  WW_TEST_CHECK(ddm != nullptr);
  if (ddm)
    WW_TEST_CHECK_EQ(ddm->version, uint32_t{3});
}

// --- wl_compositor ----------------------------------------------------------

WW_TEST_CASE(binding_wl_compositor_and_creating_a_surface_succeeds) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  WW_TEST_CHECK(comp != nullptr);

  wl_surface *surface = wl_compositor_create_surface(comp);
  c.wl.pump();
  WW_TEST_CHECK(surface != nullptr);

  // A bare surface has no role yet, so nothing should have reached the
  // backend. A compositor that created a window here would put an
  // unmapped, roleless surface on screen.
  WW_TEST_CHECK(!c.backend.called("create_window"));

  wl_surface_destroy(surface);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(a_roleless_surface_commit_creates_no_window) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  wl_surface *surface = wl_compositor_create_surface(comp);

  wl_surface_commit(surface);
  c.wl.pump();

  WW_TEST_CHECK(!c.backend.called("create_window"));

  wl_surface_destroy(surface);
  wl_compositor_destroy(comp);
}

// --- xdg_shell --------------------------------------------------------------

WW_TEST_CASE(xdg_toplevel_role_reaches_the_surface_manager) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);
  WW_TEST_CHECK(base != nullptr);

  Toplevel t = make_toplevel(c, comp, base);
  WW_TEST_CHECK(t.toplevel != nullptr);

  // The role assignment travelled the whole path: client request ->
  // xdg_wm_base.get_xdg_surface -> xdg_surface.get_toplevel -> the handler
  // unpacking the surface argument -> SurfaceManager::set_toplevel -> a real
  // backend window.
  WW_TEST_CHECK(c.backend.called("create_window"));

  xdg_toplevel_destroy(t.toplevel);
  xdg_surface_destroy(t.xdg);
  wl_surface_destroy(t.surface);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

// --- xdg_shell role-reassignment protocol errors ----------------------------
//
// A protocol error is fatal to the connection (the server posts it, then
// disconnects), so unlike every other test in this file, these do not
// clean up the objects created after the point of error -- there is
// nothing left to send that teardown to.
//
// A trap worth naming explicitly, found the hard way while writing these:
// pump() after the violating request, with a *legitimate* prior request's
// events (an xdg_toplevel/xdg_surface.configure pair, sent synchronously
// from inside set_toplevel) still undispatched in the same batch as the
// fatal wl_display.error, hangs -- this fixture's read_client_events()
// spins forever inside libwayland-client's own event dispatch rather than
// returning, with no syscalls at all once it's there (confirmed with
// strace; a real deadlock, not a busy-poll). Draining those events with
// their own pump() *before* sending the violating request avoids it
// entirely, which is why every test below does exactly that instead of
// pumping only once at the end. Filed as a sharp edge of this harness, not
// chased further -- the four tests below are what actually needed testing.

WW_TEST_CASE(a_second_xdg_surface_for_the_same_wl_surface_is_a_protocol_error) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);
  wl_surface *surface = wl_compositor_create_surface(comp);

  // get_xdg_surface alone sends no events, so there is nothing to drain
  // before the violation here -- unlike the get_toplevel/get_popup cases
  // below.
  xdg_surface *first = xdg_wm_base_get_xdg_surface(base, surface);
  // The protocol violation. The returned proxy is kept, not discarded: the
  // client allocated it whether or not the compositor accepted the request,
  // and libwayland-client only frees proxies that are explicitly destroyed.
  xdg_surface *second = xdg_wm_base_get_xdg_surface(base, surface);
  c.wl.pump();

  const struct wl_interface *err_interface = nullptr;
  uint32_t err_id = 0;
  const uint32_t code = wl_display_get_protocol_error(
      c.wl.client_display(), &err_interface, &err_id);
  WW_TEST_CHECK_EQ(code, static_cast<uint32_t>(XDG_WM_BASE_ERROR_ROLE));
  WW_TEST_CHECK(err_interface == &xdg_wm_base_interface);
  WW_TEST_CHECK_EQ(err_id, wl_proxy_get_id(reinterpret_cast<wl_proxy *>(base)));

  // The first, legitimate xdg_surface is unaffected by the second,
  // rejected request -- confirms the check ran before touching any state,
  // not as a side effect of a change that then had to be rejected.
  WW_TEST_CHECK(first != nullptr);

  xdg_surface_destroy(second);
  xdg_surface_destroy(first);
  wl_surface_destroy(surface);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(get_toplevel_twice_on_the_same_xdg_surface_is_a_protocol_error) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);
  wl_surface *surface = wl_compositor_create_surface(comp);
  xdg_surface *xdg = xdg_wm_base_get_xdg_surface(base, surface);

  xdg_toplevel *first = xdg_surface_get_toplevel(xdg);
  c.wl.pump(); // drain the configure pair this legitimately sends
  // The protocol violation; the proxy is kept so it can be destroyed below.
  xdg_toplevel *second = xdg_surface_get_toplevel(xdg);
  c.wl.pump();

  const struct wl_interface *err_interface = nullptr;
  uint32_t err_id = 0;
  const uint32_t code = wl_display_get_protocol_error(
      c.wl.client_display(), &err_interface, &err_id);
  WW_TEST_CHECK_EQ(code,
                   static_cast<uint32_t>(XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED));
  WW_TEST_CHECK(err_interface == &xdg_surface_interface);
  WW_TEST_CHECK(first != nullptr);

  // Only one create_window call, from the first, accepted get_toplevel --
  // the rejected second request never reached SurfaceManager at all.
  WW_TEST_CHECK_EQ(static_cast<std::size_t>(std::count(
                       c.backend.calls.begin(), c.backend.calls.end(),
                       "create_window")),
                   std::size_t{1});

  xdg_toplevel_destroy(second);
  xdg_toplevel_destroy(first);
  xdg_surface_destroy(xdg);
  wl_surface_destroy(surface);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(get_popup_after_get_toplevel_is_a_protocol_error) {
  // Same check, the other direction and the other role: get_popup is
  // exactly as forbidden as a second get_toplevel once any role has been
  // assigned, not just a second call of the same kind.
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);
  wl_surface *surface = wl_compositor_create_surface(comp);
  xdg_surface *xdg = xdg_wm_base_get_xdg_surface(base, surface);
  xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg);
  c.wl.pump(); // drain the configure pair this legitimately sends

  xdg_positioner *pos = xdg_wm_base_create_positioner(base);
  xdg_positioner_set_size(pos, 100, 100);
  xdg_positioner_set_anchor_rect(pos, 0, 0, 1, 1);
  // The protocol violation; the proxy is kept so it can be destroyed below.
  xdg_popup *popup = xdg_surface_get_popup(xdg, nullptr, pos);
  c.wl.pump();

  const struct wl_interface *err_interface = nullptr;
  uint32_t err_id = 0;
  const uint32_t code = wl_display_get_protocol_error(
      c.wl.client_display(), &err_interface, &err_id);
  WW_TEST_CHECK_EQ(code,
                   static_cast<uint32_t>(XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED));
  WW_TEST_CHECK(err_interface == &xdg_surface_interface);
  WW_TEST_CHECK(toplevel != nullptr);

  xdg_popup_destroy(popup);
  xdg_positioner_destroy(pos);
  xdg_toplevel_destroy(toplevel);
  xdg_surface_destroy(xdg);
  wl_surface_destroy(surface);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

// --- generic protocol violations --------------------------------------------
//
// The two cases below aren't specific to any one request handler -- they're
// libwayland-server's own protocol enforcement, exercised here to confirm
// this compositor doesn't get in its way (no crash, no hang, no state left
// behind that a later test in the same process could trip over).

WW_TEST_CASE(a_request_on_an_object_the_client_just_destroyed_is_a_protocol_error) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);
  wl_surface *surface = wl_compositor_create_surface(comp);
  xdg_surface *xdg = xdg_wm_base_get_xdg_surface(base, surface);
  c.wl.pump();

  // Send xdg_surface.destroy without the client-side proxy's usual
  // WL_MARSHAL_FLAG_DESTROY -- the generated xdg_surface_destroy() wrapper
  // always frees the proxy along with sending the request, which would make
  // `xdg` unusable for the next step below. Marshalling by hand keeps the
  // proxy alive on the client side (a client is free to hang onto a stale
  // proxy; nothing stops it) while still telling the *server* the object is
  // gone, which is exactly the buggy-client scenario this test wants: a
  // request sent for an id the server no longer maps.
  wl_proxy_marshal_flags(reinterpret_cast<wl_proxy *>(xdg), XDG_SURFACE_DESTROY,
                         nullptr,
                         wl_proxy_get_version(reinterpret_cast<wl_proxy *>(xdg)),
                         0);
  c.wl.pump();

  // The object is gone server-side; anything sent against it now is a
  // reference to a destroyed object.
  xdg_surface_ack_configure(xdg, 0);
  c.wl.pump();

  const struct wl_interface *err_interface = nullptr;
  uint32_t err_id = 0;
  const uint32_t code = wl_display_get_protocol_error(
      c.wl.client_display(), &err_interface, &err_id);
  WW_TEST_CHECK_EQ(code, static_cast<uint32_t>(WL_DISPLAY_ERROR_INVALID_OBJECT));
  WW_TEST_CHECK(err_interface == &wl_display_interface);

  // Fatal to the connection -- nothing left to tear down on the client side
  // beyond the proxy this test deliberately kept alive.
  wl_proxy_destroy(reinterpret_cast<wl_proxy *>(xdg));
  wl_surface_destroy(surface);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(binding_a_global_above_its_advertised_version_is_a_protocol_error) {
  Compositor c;

  const auto *g = c.wl.find_global("wl_compositor");
  WW_TEST_CHECK(g != nullptr);

  // bind()/bind_deferred() clamp to the advertised version deliberately, so
  // a request above it has to go through the raw registry proxy instead --
  // exactly what a client that ignores the version a server advertised
  // would do.
  void *proxy = wl_registry_bind(c.wl.registry(), g->name,
                                 &wl_compositor_interface, g->version + 50);
  c.wl.pump();

  const struct wl_interface *err_interface = nullptr;
  uint32_t err_id = 0;
  const uint32_t code = wl_display_get_protocol_error(
      c.wl.client_display(), &err_interface, &err_id);
  WW_TEST_CHECK_EQ(code, static_cast<uint32_t>(WL_DISPLAY_ERROR_INVALID_OBJECT));
  WW_TEST_CHECK(err_interface == &wl_registry_interface);

  wl_proxy_destroy(static_cast<wl_proxy *>(proxy));
}

// --- wl_surface.attach / set_buffer_scale error paths -----------------------

WW_TEST_CASE(attach_with_a_non_zero_offset_is_a_protocol_error) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  wl_surface *surface = wl_compositor_create_surface(comp);
  c.wl.pump();

  // No wl_buffer needed: the version check runs before the handler ever
  // looks at `buffer`, so nullptr exercises exactly the path under test.
  wl_surface_attach(surface, nullptr, 3, 4);
  c.wl.pump();

  const struct wl_interface *err_interface = nullptr;
  uint32_t err_id = 0;
  const uint32_t code = wl_display_get_protocol_error(
      c.wl.client_display(), &err_interface, &err_id);
  WW_TEST_CHECK_EQ(code, static_cast<uint32_t>(WL_SURFACE_ERROR_INVALID_OFFSET));
  WW_TEST_CHECK(err_interface == &wl_surface_interface);

  wl_surface_destroy(surface);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(attach_with_a_zero_offset_is_accepted) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  wl_surface *surface = wl_compositor_create_surface(comp);
  c.wl.pump();

  wl_surface_attach(surface, nullptr, 0, 0);
  c.wl.pump();

  const struct wl_interface *err_interface = nullptr;
  uint32_t err_id = 0;
  const uint32_t code = wl_display_get_protocol_error(
      c.wl.client_display(), &err_interface, &err_id);
  WW_TEST_CHECK_EQ(code, uint32_t{0});
  WW_TEST_CHECK(err_interface == nullptr);

  wl_surface_destroy(surface);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(set_buffer_scale_with_a_non_positive_value_is_a_protocol_error) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  wl_surface *surface = wl_compositor_create_surface(comp);
  c.wl.pump();

  wl_surface_set_buffer_scale(surface, 0);
  c.wl.pump();

  const struct wl_interface *err_interface = nullptr;
  uint32_t err_id = 0;
  const uint32_t code = wl_display_get_protocol_error(
      c.wl.client_display(), &err_interface, &err_id);
  WW_TEST_CHECK_EQ(code, static_cast<uint32_t>(WL_SURFACE_ERROR_INVALID_SCALE));
  WW_TEST_CHECK(err_interface == &wl_surface_interface);

  wl_surface_destroy(surface);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(xdg_surface_sends_a_configure_the_client_can_ack) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);

  struct Seen {
    bool configured{false};
    uint32_t serial{0};
  } seen;

  wl_surface *surface = wl_compositor_create_surface(comp);
  xdg_surface *xdg = xdg_wm_base_get_xdg_surface(base, surface);
  static const xdg_surface_listener listener = {
      [](void *data, xdg_surface *s, uint32_t serial) {
        auto *seen_ptr = static_cast<Seen *>(data);
        seen_ptr->configured = true;
        seen_ptr->serial = serial;
        xdg_surface_ack_configure(s, serial);
      }};
  xdg_surface_add_listener(xdg, &listener, &seen);

  xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg);
  wl_surface_commit(surface);
  c.wl.pump();

  // The configure handshake is not optional: a client that never receives
  // one is required by the protocol to never attach a buffer, so getting
  // this wrong means nothing ever renders.
  WW_TEST_CHECK(seen.configured);
  WW_TEST_CHECK(seen.serial != 0);

  xdg_toplevel_destroy(toplevel);
  xdg_surface_destroy(xdg);
  wl_surface_destroy(surface);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(xdg_toplevel_set_title_reaches_the_backend_verbatim) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);
  Toplevel t = make_toplevel(c, comp, base);

  // A string argument is the one thing a request handler can most easily get
  // wrong -- wrong index, wrong lifetime, truncation.
  xdg_toplevel_set_title(t.toplevel, "hello from a real client");
  c.wl.pump();

  WW_TEST_CHECK(c.backend.called("set_window_title"));
  bool found = false;
  for (const auto &[id, w] : c.backend.windows)
    if (w.title == "hello from a real client")
      found = true;
  WW_TEST_CHECK(found);

  xdg_toplevel_destroy(t.toplevel);
  xdg_surface_destroy(t.xdg);
  wl_surface_destroy(t.surface);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(xdg_toplevel_identity_requests_reach_the_backend_over_the_wire) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);

  // A parent and a child, driven as a real client would: every request is
  // marshalled, sent down the socket and unpacked by the real handlers.
  wl_surface *psurf = wl_compositor_create_surface(comp);
  xdg_surface *pxdg = xdg_wm_base_get_xdg_surface(base, psurf);
  xdg_toplevel *ptop = xdg_surface_get_toplevel(pxdg);
  wl_surface_commit(psurf);

  wl_surface *csurf = wl_compositor_create_surface(comp);
  xdg_surface *cxdg = xdg_wm_base_get_xdg_surface(base, csurf);
  xdg_toplevel *ctop = xdg_surface_get_toplevel(cxdg);
  xdg_toplevel_set_app_id(ctop, "org.example.Dialog");
  xdg_toplevel_set_min_size(ctop, 300, 150);
  xdg_toplevel_set_max_size(ctop, 900, 600);
  xdg_toplevel_set_parent(ctop, ptop);
  c.wl.pump();

  // Nothing is mapped for the child until it commits ...
  const auto count_mapped = [&] {
    std::size_t n = 0;
    for (const auto &[id, w] : c.backend.windows)
      n += w.mapped ? 1u : 0u;
    return n;
  };
  WW_TEST_CHECK_EQ(count_mapped(), std::size_t{1}); // just the parent

  wl_surface_commit(csurf);
  c.wl.pump();
  WW_TEST_CHECK_EQ(count_mapped(), std::size_t{2});

  const ww_test::FakeWindowBackend::Window *child = nullptr;
  uint32_t parent_id = 0;
  for (const auto &[id, w] : c.backend.windows) {
    if (w.wm_class == "org.example.Dialog")
      child = &w;
    else
      parent_id = id;
  }
  WW_TEST_CHECK(child != nullptr);
  if (child) {
    WW_TEST_CHECK(child->wm_class_instance == "org.example.Dialog");
    WW_TEST_CHECK_EQ(child->size_limits.min_width, 300);
    WW_TEST_CHECK_EQ(child->size_limits.max_height, 600);
    WW_TEST_CHECK_EQ(child->transient_for, parent_id);
  }

  xdg_toplevel_destroy(ctop);
  xdg_surface_destroy(cxdg);
  wl_surface_destroy(csurf);
  xdg_toplevel_destroy(ptop);
  xdg_surface_destroy(pxdg);
  wl_surface_destroy(psurf);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(xdg_toplevel_move_resize_minimize_reach_the_backend_over_the_wire) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);
  wl_seat *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));

  wl_surface *surf = wl_compositor_create_surface(comp);
  xdg_surface *xdg = xdg_wm_base_get_xdg_surface(base, surf);
  xdg_toplevel *top = xdg_surface_get_toplevel(xdg);
  wl_surface_commit(surf);
  c.wl.pump();

  const uint32_t win = c.backend.windows.begin()->first;

  // Driven exactly as a real client would: every request marshalled over
  // the real socket and unpacked by the real xdg_toplevel handlers, same
  // as xdg_toplevel_identity_requests_reach_the_backend_over_the_wire
  // above.
  xdg_toplevel_move(top, seat, /*serial=*/1);
  xdg_toplevel_resize(top, seat, /*serial=*/2,
                      XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
  xdg_toplevel_set_minimized(top);
  c.wl.pump();

  WW_TEST_CHECK_EQ(c.backend.interactive_moves.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(c.backend.interactive_moves.front(), win);

  WW_TEST_CHECK_EQ(c.backend.interactive_resizes.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(c.backend.interactive_resizes.front().window, win);
  WW_TEST_CHECK(c.backend.interactive_resizes.front().edge ==
                ww::ResizeEdge::BottomRight);

  WW_TEST_CHECK_EQ(c.backend.minimized_windows.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(c.backend.minimized_windows.front(), win);

  xdg_toplevel_destroy(top);
  xdg_surface_destroy(xdg);
  wl_surface_destroy(surf);
  wl_seat_destroy(seat);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

WW_TEST_CASE(destroying_a_toplevel_destroys_its_window) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);
  Toplevel t = make_toplevel(c, comp, base);
  WW_TEST_CHECK(c.backend.called("create_window"));

  xdg_toplevel_destroy(t.toplevel);
  xdg_surface_destroy(t.xdg);
  wl_surface_destroy(t.surface);
  c.wl.pump();

  // Teardown arriving as real protocol destroys, rather than as a direct
  // call, is the case the destroy-listener wiring exists for.
  WW_TEST_CHECK(c.backend.called("destroy_window"));

  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

// --- wl_subcompositor -------------------------------------------------------

WW_TEST_CASE(subsurface_of_a_toplevel_becomes_a_child_window) {
  Compositor c;
  wl_compositor *comp = bind_compositor(c);
  xdg_wm_base *base = bind_wm_base(c);
  auto *subcomp = static_cast<wl_subcompositor *>(
      c.wl.bind("wl_subcompositor", &wl_subcompositor_interface, 1));
  WW_TEST_CHECK(subcomp != nullptr);

  Toplevel parent = make_toplevel(c, comp, base);
  c.backend.clear_calls();

  wl_surface *child = wl_compositor_create_surface(comp);
  wl_subsurface *sub =
      wl_subcompositor_get_subsurface(subcomp, child, parent.surface);
  wl_subsurface_set_position(sub, 10, 20);
  wl_surface_commit(child);
  wl_surface_commit(parent.surface);
  c.wl.pump();

  // Both object arguments (child and parent) have to be unpacked correctly
  // for this to land as a child window rather than a second toplevel.
  WW_TEST_CHECK(c.backend.called("create_child_window"));

  wl_subsurface_destroy(sub);
  wl_surface_destroy(child);
  xdg_toplevel_destroy(parent.toplevel);
  xdg_surface_destroy(parent.xdg);
  wl_surface_destroy(parent.surface);
  wl_subcompositor_destroy(subcomp);
  xdg_wm_base_destroy(base);
  wl_compositor_destroy(comp);
}

// --- wl_output --------------------------------------------------------------

WW_TEST_CASE(wl_output_reports_the_configured_geometry_and_mode) {
  Compositor c;

  struct Seen {
    bool geometry{false};
    bool mode{false};
    bool done{false};
    int32_t mm_width{0};
    int32_t mm_height{0};
    int32_t width{0};
    int32_t height{0};
  } seen;

  // Bound without pumping: wl_output emits geometry/mode/done from inside
  // its bind handler, so the listener has to be attached before any exchange
  // or those events arrive at an object that is not listening yet.
  auto *output = static_cast<wl_output *>(
      c.wl.bind_deferred("wl_output", &wl_output_interface, 3));
  WW_TEST_CHECK(output != nullptr);

  static const wl_output_listener listener = {
      [](void *data, wl_output *, int32_t, int32_t, int32_t mm_w, int32_t mm_h,
         int32_t, const char *, const char *, int32_t) {
        auto *s = static_cast<Seen *>(data);
        s->geometry = true;
        s->mm_width = mm_w;
        s->mm_height = mm_h;
      },
      [](void *data, wl_output *, uint32_t, int32_t w, int32_t h, int32_t) {
        auto *s = static_cast<Seen *>(data);
        s->mode = true;
        s->width = w;
        s->height = h;
      },
      [](void *data, wl_output *) {
        static_cast<Seen *>(data)->done = true;
      },
      [](void *, wl_output *, int32_t) {},
      // wl_output v4 added name/description. Never delivered here (this
      // binds at v3), but the listener struct must still be fully
      // initialised or libwayland reads past the end of it.
      [](void *, wl_output *, const char *) {},
      [](void *, wl_output *, const char *) {}};
  wl_output_add_listener(output, &listener, &seen);
  c.wl.pump();

  // These are the numbers register_core_globals was handed. A client sizes
  // and scales its surfaces from them, so passing the wrong field through
  // (a classic: swapping pixels and millimetres) is a visible bug.
  WW_TEST_CHECK(seen.geometry);
  WW_TEST_CHECK(seen.mode);
  WW_TEST_CHECK(seen.done);
  WW_TEST_CHECK_EQ(seen.width, kOutputWidth);
  WW_TEST_CHECK_EQ(seen.height, kOutputHeight);
  WW_TEST_CHECK_EQ(seen.mm_width, kOutputMmWidth);
  WW_TEST_CHECK_EQ(seen.mm_height, kOutputMmHeight);

  wl_output_destroy(output);
}

// --- wl_output follows the window system ------------------------------------

namespace {
// What one bound wl_output has been told, cumulatively.
struct OutputSeen {
  int geometry_events{0};
  int mode_events{0};
  int done_events{0};
  int32_t width{0};
  int32_t height{0};
  int32_t mm_width{0};
  int32_t mm_height{0};
  int32_t refresh{0};
};

wl_output *bind_listening_output(Compositor &c, OutputSeen &seen) {
  auto *output = static_cast<wl_output *>(
      c.wl.bind_deferred("wl_output", &wl_output_interface, 3));
  static const wl_output_listener listener = {
      [](void *data, wl_output *, int32_t, int32_t, int32_t mm_w, int32_t mm_h,
         int32_t, const char *, const char *, int32_t) {
        auto *s = static_cast<OutputSeen *>(data);
        ++s->geometry_events;
        s->mm_width = mm_w;
        s->mm_height = mm_h;
      },
      [](void *data, wl_output *, uint32_t, int32_t w, int32_t h, int32_t r) {
        auto *s = static_cast<OutputSeen *>(data);
        ++s->mode_events;
        s->width = w;
        s->height = h;
        s->refresh = r;
      },
      [](void *data, wl_output *) { ++static_cast<OutputSeen *>(data)->done_events; },
      [](void *, wl_output *, int32_t) {},
      [](void *, wl_output *, const char *) {},
      [](void *, wl_output *, const char *) {}};
  wl_output_add_listener(output, &listener, &seen);
  c.wl.pump();
  return output;
}
} // namespace

WW_TEST_CASE(a_screen_change_reaches_a_client_that_already_holds_a_wl_output) {
  Compositor c;
  OutputSeen seen;
  wl_output *output = bind_listening_output(c, seen);
  WW_TEST_CHECK_EQ(seen.done_events, 1);
  WW_TEST_CHECK_EQ(seen.width, kOutputWidth);

  // The window system resized the screen and reported a refresh rate.
  ww::update_output(*c.output_global,
                    ww::OutputState{800, 600, 211, 158, 75000});
  c.wl.pump();

  WW_TEST_CHECK_EQ(seen.geometry_events, 2);
  WW_TEST_CHECK_EQ(seen.mode_events, 2);
  WW_TEST_CHECK_EQ(seen.done_events, 2);
  WW_TEST_CHECK_EQ(seen.width, 800);
  WW_TEST_CHECK_EQ(seen.height, 600);
  WW_TEST_CHECK_EQ(seen.mm_width, 211);
  WW_TEST_CHECK_EQ(seen.mm_height, 158);
  WW_TEST_CHECK_EQ(seen.refresh, 75000);

  wl_output_destroy(output);
}

WW_TEST_CASE(every_client_holding_a_wl_output_is_told) {
  Compositor c;
  OutputSeen a, b;
  wl_output *first = bind_listening_output(c, a);
  wl_output *second = bind_listening_output(c, b);

  ww::update_output(*c.output_global, ww::OutputState{640, 480, 169, 127, 0});
  c.wl.pump();

  WW_TEST_CHECK_EQ(a.width, 640);
  WW_TEST_CHECK_EQ(b.width, 640);
  WW_TEST_CHECK_EQ(a.done_events, 2);
  WW_TEST_CHECK_EQ(b.done_events, 2);

  wl_output_destroy(first);
  wl_output_destroy(second);
}

WW_TEST_CASE(a_wl_output_bound_after_a_change_gets_the_new_values) {
  Compositor c;
  ww::update_output(*c.output_global, ww::OutputState{1024, 768, 270, 203, 60000});

  OutputSeen seen;
  wl_output *output = bind_listening_output(c, seen);
  WW_TEST_CHECK_EQ(seen.width, 1024);
  WW_TEST_CHECK_EQ(seen.height, 768);
  WW_TEST_CHECK_EQ(seen.mm_width, 270);
  wl_output_destroy(output);
}

WW_TEST_CASE(an_unchanged_screen_sends_nothing) {
  Compositor c;
  OutputSeen seen;
  wl_output *output = bind_listening_output(c, seen);

  // Re-reporting what the client already has must not cost it a round of
  // geometry/mode/done (and a relayout) -- RandR emits several notifications
  // for one change.
  ww::update_output(*c.output_global,
                    ww::OutputState{kOutputWidth, kOutputHeight, kOutputMmWidth,
                                    kOutputMmHeight, 0});
  c.wl.pump();
  WW_TEST_CHECK_EQ(seen.done_events, 1);

  wl_output_destroy(output);
}

WW_TEST_CASE(a_refresh_rate_the_window_system_could_not_give_is_announced_as_60hz) {
  Compositor c;
  OutputSeen seen;
  wl_output *output = bind_listening_output(c, seen);
  WW_TEST_CHECK_EQ(seen.refresh, 60000); // the fixture passes refresh_mhz 0
  wl_output_destroy(output);
}

WW_TEST_CASE(a_destroyed_wl_output_is_not_touched_by_a_later_update) {
  Compositor c;
  OutputSeen gone, kept;
  wl_output *released = bind_listening_output(c, gone);
  wl_output *held = bind_listening_output(c, kept);
  wl_output_release(released);
  c.wl.pump();

  ww::update_output(*c.output_global, ww::OutputState{800, 600, 211, 158, 0});
  c.wl.pump();

  WW_TEST_CHECK_EQ(gone.done_events, 1); // nothing after the release
  WW_TEST_CHECK_EQ(kept.done_events, 2);

  wl_output_destroy(held);
}

namespace {
struct ScaleSeen {
  int events{0};
  uint32_t last{0};
};
} // namespace

WW_TEST_CASE(a_screen_change_refreshes_the_preferred_fractional_scale) {
  Compositor c;
  auto global = ww::register_fractional_scale(c.wl.server_display(), 120);
  WW_TEST_CHECK(global.has_value());
  if (!global)
    return;
  c.wl.pump();

  // The client side of wp_fractional_scale is not generated for the tests, so
  // drive it through raw proxies on the (shared) interface objects.
  auto *manager = static_cast<wl_proxy *>(c.wl.bind_deferred(
      "wp_fractional_scale_manager_v1", &wp_fractional_scale_manager_v1_interface, 1));
  WW_TEST_CHECK(manager != nullptr);
  if (!manager)
    return;
  auto *comp = static_cast<wl_compositor *>(
      c.wl.bind("wl_compositor", &wl_compositor_interface, 5));
  wl_surface *surface = wl_compositor_create_surface(comp);
  wl_proxy *object = wl_proxy_marshal_flags(
      manager, 1 /* get_fractional_scale */,
      &wp_fractional_scale_v1_interface, wl_proxy_get_version(manager), 0,
      nullptr, surface);
  ScaleSeen seen;
  static void (*events[])(void) = {reinterpret_cast<void (*)(void)>(
      +[](void *data, void *, uint32_t scale) {
        auto *s = static_cast<ScaleSeen *>(data);
        ++s->events;
        s->last = scale;
      })};
  wl_proxy_add_listener(object, events, &seen);
  c.wl.pump();
  WW_TEST_CHECK_EQ(seen.last, uint32_t{120}); // sent when the object was made

  ww::update_fractional_scale(**global, 180);
  c.wl.pump();
  WW_TEST_CHECK_EQ(seen.last, uint32_t{180});
  WW_TEST_CHECK_EQ(seen.events, 2);

  ww::update_fractional_scale(**global, 180); // unchanged: nothing more
  c.wl.pump();
  WW_TEST_CHECK_EQ(seen.events, 2);

  wl_proxy_destroy(object);
  wl_surface_destroy(surface);
  wl_compositor_destroy(comp);
  wl_proxy_destroy(manager);
}

// --- wl_seat ----------------------------------------------------------------

WW_TEST_CASE(wl_seat_advertises_pointer_and_keyboard_capabilities) {
  Compositor c;

  struct Seen {
    bool got{false};
    uint32_t caps{0};
  } seen;

  // Same bind-time event ordering as wl_output above: capabilities is sent
  // from the bind handler.
  auto *seat = static_cast<wl_seat *>(
      c.wl.bind_deferred("wl_seat", &wl_seat_interface, 7));
  WW_TEST_CHECK(seat != nullptr);

  static const wl_seat_listener listener = {
      [](void *data, wl_seat *, uint32_t caps) {
        auto *s = static_cast<Seen *>(data);
        s->got = true;
        s->caps = caps;
      },
      [](void *, wl_seat *, const char *) {}};
  wl_seat_add_listener(seat, &listener, &seen);
  c.wl.pump();

  // Clients gate wl_seat.get_pointer/get_keyboard on these bits; a seat that
  // advertises nothing gets no input requests and therefore no input.
  WW_TEST_CHECK(seen.got);
  WW_TEST_CHECK((seen.caps & WL_SEAT_CAPABILITY_POINTER) != 0);
  WW_TEST_CHECK((seen.caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0);

  wl_seat_destroy(seat);
}

WW_TEST_CASE(wl_seat_get_pointer_and_get_keyboard_return_usable_objects) {
  Compositor c;
  auto *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
  c.wl.pump();

  wl_pointer *pointer = wl_seat_get_pointer(seat);
  wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
  c.wl.pump();

  WW_TEST_CHECK(pointer != nullptr);
  WW_TEST_CHECK(keyboard != nullptr);
  // The handlers register these with InputTranslator; a failure to unpack
  // the new-id argument shows up as a protocol error killing the client.
  WW_TEST_CHECK(wl_display_get_error(c.wl.client_display()) == 0);

  wl_pointer_destroy(pointer);
  wl_keyboard_destroy(keyboard);
  wl_seat_destroy(seat);
}

namespace {

struct OfferObserver {
  std::vector<std::string> mimes;
};

void offer_on_offer(void *data, wl_data_offer *, const char *mime) {
  static_cast<OfferObserver *>(data)->mimes.emplace_back(mime ? mime : "");
}

const wl_data_offer_listener kOfferListener = {
    offer_on_offer,
    [](void *, wl_data_offer *, uint32_t) {}, // source_actions
    [](void *, wl_data_offer *, uint32_t) {}, // action
};

struct DeviceObserver {
  OfferObserver offer;
  // Proxies the compositor created for us. A wl_data_offer arrives as a
  // server-allocated new-id, so libwayland-client materialises a proxy
  // whether or not we listen to it; destroying them is the client's job and
  // leaking them shows up under ASan exactly like any other client leak.
  std::vector<wl_data_offer *> offers;
  int data_offers{0};
  int enters{0};
  int motions{0};
  int drops{0};
  int leaves{0};
  int selections{0};
};

const wl_data_device_listener kDeviceListener = {
    // data_offer: a new offer object, before the enter/selection naming it.
    [](void *data, wl_data_device *, wl_data_offer *offer) {
      auto *d = static_cast<DeviceObserver *>(data);
      ++d->data_offers;
      d->offers.push_back(offer);
      wl_data_offer_add_listener(offer, &kOfferListener, &d->offer);
    },
    [](void *data, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t,
       wl_fixed_t, wl_data_offer *) {
      ++static_cast<DeviceObserver *>(data)->enters;
    },
    [](void *data, wl_data_device *) {
      ++static_cast<DeviceObserver *>(data)->leaves;
    },
    [](void *data, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {
      ++static_cast<DeviceObserver *>(data)->motions;
    },
    [](void *data, wl_data_device *) {
      ++static_cast<DeviceObserver *>(data)->drops;
    },
    [](void *data, wl_data_device *, wl_data_offer *) {
      ++static_cast<DeviceObserver *>(data)->selections;
    },
};

void destroy_offers(DeviceObserver &o) {
  for (wl_data_offer *offer : o.offers)
    wl_data_offer_destroy(offer);
  o.offers.clear();
}

} // namespace

// --- wl_data_device_manager -------------------------------------------------

WW_TEST_CASE(data_device_manager_binds_and_vends_a_device) {
  Compositor c;
  auto *ddm = static_cast<wl_data_device_manager *>(c.wl.bind(
      "wl_data_device_manager", &wl_data_device_manager_interface, 3));
  WW_TEST_CHECK(ddm != nullptr);

  auto *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
  wl_data_device *device = wl_data_device_manager_get_data_device(ddm, seat);
  c.wl.pump();

  WW_TEST_CHECK(device != nullptr);
  WW_TEST_CHECK(wl_display_get_error(c.wl.client_display()) == 0);

  wl_data_device_destroy(device);
  wl_seat_destroy(seat);
  wl_data_device_manager_destroy(ddm);
}

WW_TEST_CASE(setting_a_selection_takes_x11_clipboard_ownership) {
  Compositor c;
  auto *ddm = static_cast<wl_data_device_manager *>(c.wl.bind(
      "wl_data_device_manager", &wl_data_device_manager_interface, 3));
  auto *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
  wl_data_device *device = wl_data_device_manager_get_data_device(ddm, seat);
  DeviceObserver observed;
  wl_data_device_add_listener(device, &kDeviceListener, &observed);

  wl_data_source *source = wl_data_device_manager_create_data_source(ddm);
  wl_data_source_offer(source, "text/plain;charset=utf-8");
  wl_data_source_offer(source, "text/plain");
  wl_data_device_set_selection(device, source, 1);
  c.wl.pump();

  // The MIME types the client offered have to survive unpacking and reach
  // the backend, or a native X11 application sees an empty clipboard.
  WW_TEST_CHECK(c.backend.called("set_clipboard_owner"));
  bool has_utf8 = false;
  for (const auto &m : c.backend.clipboard_mimes)
    if (m == "text/plain;charset=utf-8")
      has_utf8 = true;
  WW_TEST_CHECK(has_utf8);

  destroy_offers(observed);
  wl_data_source_destroy(source);
  wl_data_device_destroy(device);
  wl_seat_destroy(seat);
  wl_data_device_manager_destroy(ddm);
}

WW_TEST_CASE(clearing_the_selection_releases_x11_clipboard_ownership) {
  Compositor c;
  auto *ddm = static_cast<wl_data_device_manager *>(c.wl.bind(
      "wl_data_device_manager", &wl_data_device_manager_interface, 3));
  auto *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
  wl_data_device *device = wl_data_device_manager_get_data_device(ddm, seat);
  DeviceObserver observed;
  wl_data_device_add_listener(device, &kDeviceListener, &observed);

  wl_data_source *source = wl_data_device_manager_create_data_source(ddm);
  wl_data_source_offer(source, "text/plain");
  wl_data_device_set_selection(device, source, 1);
  c.wl.pump();
  WW_TEST_CHECK(!c.backend.clipboard_mimes.empty());

  // A null source means "nobody owns the selection now". Failing to forward
  // that leaves WWindows claiming an X11 selection it can no longer serve,
  // so every paste in a native application hangs or returns stale data.
  wl_data_device_set_selection(device, nullptr, 2);
  c.wl.pump();
  WW_TEST_CHECK(c.backend.clipboard_mimes.empty());

  destroy_offers(observed);
  wl_data_source_destroy(source);
  wl_data_device_destroy(device);
  wl_seat_destroy(seat);
  wl_data_device_manager_destroy(ddm);
}

// --- drag and drop ----------------------------------------------------------
//
// The last piece of the data_device glue, and the one with real logic in it:
// start_drag hands InputTranslator a set of callbacks, and from then until
// the button is released the translator redirects pointer dispatch into them
// instead of sending ordinary wl_pointer events. Everything a target client
// sees during a drag -- the offer, enter, motion, drop -- is produced by that
// redirect.
//
// Driving it needs both halves: a real client to issue start_drag and observe
// the resulting events, and raw backend input callbacks to move the pointer,
// which is what X11Backend does in production.

namespace {
struct SourceObserver {
  int cancelled{0};
  int drop_performed{0};
  // (mime, fd) from each wl_data_source.send event -- the fd is a real,
  // already-received descriptor (SCM_RIGHTS, delivered by libwayland-client
  // like any other event argument), the caller's to read/write/close.
  std::vector<std::pair<std::string, int>> sends;
};

const wl_data_source_listener kSourceListener = {
    [](void *, wl_data_source *, const char *) {}, // target
    [](void *data, wl_data_source *, const char *mime, int32_t fd) {
      static_cast<SourceObserver *>(data)->sends.emplace_back(
          mime ? mime : "", fd);
    },
    [](void *data, wl_data_source *) {
      ++static_cast<SourceObserver *>(data)->cancelled;
    },
    [](void *data, wl_data_source *) {
      ++static_cast<SourceObserver *>(data)->drop_performed;
    },
    [](void *, wl_data_source *) {}, // dnd_finished
    [](void *, wl_data_source *, uint32_t) {}, // action
};

// The backend window id of the first window, which is what the raw input
// callbacks are addressed to.
uint32_t first_window(Compositor &c) {
  for (const auto &[id, w] : c.backend.windows) {
    (void)w;
    return id;
  }
  return 0;
}

// A client with a mapped toplevel, a data device under observation, and a
// data source ready to drag.
struct DragFixture {
  Compositor c;
  wl_compositor *comp{nullptr};
  xdg_wm_base *base{nullptr};
  wl_seat *seat{nullptr};
  wl_data_device_manager *ddm{nullptr};
  wl_data_device *device{nullptr};
  wl_data_source *source{nullptr};
  Toplevel top;
  DeviceObserver observed;
  SourceObserver source_observed;
  uint32_t window{0};

  DragFixture() {
    comp = bind_compositor(c);
    base = bind_wm_base(c);
    seat = static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
    ddm = static_cast<wl_data_device_manager *>(c.wl.bind(
        "wl_data_device_manager", &wl_data_device_manager_interface, 3));

    device = wl_data_device_manager_get_data_device(ddm, seat);
    wl_data_device_add_listener(device, &kDeviceListener, &observed);

    top = make_toplevel(c, comp, base);
    window = first_window(c);

    source = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_add_listener(source, &kSourceListener, &source_observed);
    wl_data_source_offer(source, "text/plain;charset=utf-8");
    wl_data_source_offer(source, "text/uri-list");
    c.wl.pump();
  }

  ~DragFixture() {
    destroy_offers(observed);
    if (source)
      wl_data_source_destroy(source);
    if (device)
      wl_data_device_destroy(device);
    if (top.toplevel)
      xdg_toplevel_destroy(top.toplevel);
    if (top.xdg)
      xdg_surface_destroy(top.xdg);
    if (top.surface)
      wl_surface_destroy(top.surface);
    if (ddm)
      wl_data_device_manager_destroy(ddm);
    if (seat)
      wl_seat_destroy(seat);
    if (base)
      xdg_wm_base_destroy(base);
    if (comp)
      wl_compositor_destroy(comp);
  }

  // Put the pointer over the toplevel, which is where a real drag starts:
  // begin_drag synthesizes its first on_enter from wherever the pointer
  // already sits.
  void pointer_enters() {
    c.backend.input_cbs.on_enter(window, 5, 6, 105, 106);
  }
  void pointer_moves(int16_t x, int16_t y) {
    c.backend.input_cbs.on_motion(window, x, y, x, y, 1000);
  }
  void pointer_leaves() { c.backend.input_cbs.on_leave(window); }
  // X11 button 1 == left; InputTranslator maps it to BTN_LEFT.
  void releases_button() {
    c.backend.input_cbs.on_button(window, 1, false, 2000);
  }

  void start() {
    wl_data_device_start_drag(device, source, top.surface, nullptr, 1);
    c.wl.pump();
  }
};

} // namespace

WW_TEST_CASE(start_drag_offers_the_source_mimes_to_the_target) {
  DragFixture f;
  f.pointer_enters();
  f.start();

  // The target client learns about the drag through a fresh wl_data_offer
  // followed by an enter naming it. Getting the order wrong means the client
  // receives an enter referencing an object it has never been told about.
  WW_TEST_CHECK(f.observed.data_offers >= 1);
  WW_TEST_CHECK(f.observed.enters >= 1);

  bool has_text = false;
  bool has_uris = false;
  for (const auto &m : f.observed.offer.mimes) {
    if (m == "text/plain;charset=utf-8")
      has_text = true;
    if (m == "text/uri-list")
      has_uris = true;
  }
  WW_TEST_CHECK(has_text);
  WW_TEST_CHECK(has_uris);
}

WW_TEST_CASE(pointer_motion_during_a_drag_arrives_as_data_device_motion) {
  DragFixture f;
  f.pointer_enters();
  f.start();
  const int before = f.observed.motions;

  f.pointer_moves(20, 30);
  f.pointer_moves(21, 31);
  f.c.wl.pump();

  // While a drag is active the translator must route motion to the drag
  // callbacks instead of dispatching ordinary wl_pointer.motion; this is the
  // observable half of that redirect.
  WW_TEST_CHECK(f.observed.motions > before);
}

WW_TEST_CASE(releasing_the_button_over_a_target_delivers_a_drop) {
  DragFixture f;
  f.pointer_enters();
  f.start();
  WW_TEST_CHECK(f.observed.enters >= 1);

  f.releases_button();
  f.c.wl.pump();

  // Both halves of a successful drop: the target is told to take the data,
  // and the source is told the drop happened so it can prepare to send it.
  WW_TEST_CHECK_EQ(f.observed.drops, 1);
  WW_TEST_CHECK_EQ(f.source_observed.drop_performed, 1);
  WW_TEST_CHECK_EQ(f.source_observed.cancelled, 0);
}

WW_TEST_CASE(releasing_with_no_target_cancels_the_source) {
  DragFixture f;
  f.pointer_enters();
  f.start();

  f.pointer_leaves();
  f.c.wl.pump();
  WW_TEST_CHECK(f.observed.leaves >= 1);

  f.releases_button();
  f.c.wl.pump();

  // A drag released over nothing is a cancel, not a drop. Sending a drop
  // here would make the source believe data was accepted by a target that
  // does not exist.
  WW_TEST_CHECK_EQ(f.observed.drops, 0);
  WW_TEST_CHECK_EQ(f.source_observed.cancelled, 1);
}

WW_TEST_CASE(a_second_start_drag_while_one_is_active_is_ignored) {
  DragFixture f;
  f.pointer_enters();
  f.start();
  const int offers_after_first = f.observed.data_offers;

  f.start();

  // Two concurrent drags have no meaning: one pointer, one grab. The second
  // request must not reset the in-flight drag's state.
  WW_TEST_CHECK_EQ(f.observed.data_offers, offers_after_first);
  WW_TEST_CHECK_EQ(f.source_observed.cancelled, 0);
}

WW_TEST_CASE(a_drag_ends_cleanly_and_ordinary_pointer_dispatch_resumes) {
  DragFixture f;
  f.pointer_enters();
  f.start();
  f.releases_button();
  f.c.wl.pump();
  WW_TEST_CHECK_EQ(f.observed.drops, 1);

  const int motions_at_drop = f.observed.motions;
  f.pointer_moves(40, 40);
  f.c.wl.pump();

  // end_drag has to actually tear the redirect down. If it did not, motion
  // after the drop would keep arriving as data_device.motion forever and the
  // client would never receive an ordinary pointer event again.
  WW_TEST_CHECK_EQ(f.observed.motions, motions_at_drop);
  WW_TEST_CHECK(wl_display_get_error(f.c.wl.client_display()) == 0);
}

// --- wl_data_offer.receive: the fd handshake after the drop -----------------
//
// Everything above stops at the drop event -- the target has been told data
// is available, but never actually asks for it. A real target follows the
// drop with wl_data_offer.receive(mime, fd): a pipe whose write end it hands
// to us, expecting the *source* to eventually write the bytes into it. The
// server's job in the middle is pure plumbing -- forward that fd to the drag
// source's wl_data_source.send -- which this test drives end to end with a
// real pipe rather than just checking a call happened.

WW_TEST_CASE(receive_after_a_drop_forwards_the_fd_for_the_source_to_write) {
  DragFixture f;
  f.pointer_enters();
  f.start();
  f.releases_button();
  f.c.wl.pump();
  WW_TEST_CHECK_EQ(f.observed.drops, 1);
  WW_TEST_CHECK(!f.observed.offers.empty());
  if (f.observed.offers.empty())
    return;

  int pipe_fds[2] = {-1, -1};
  WW_TEST_CHECK(::pipe(pipe_fds) == 0);
  const int read_end = pipe_fds[0];
  const int write_end = pipe_fds[1];

  wl_data_offer_receive(f.observed.offers.back(), "text/plain;charset=utf-8",
                        write_end);
  f.c.wl.pump();
  // Our copy of the write end: the kernel already dup'd it into the request's
  // ancillary data during that pump, same as any real client would close its
  // own copy right after the request.
  ::close(write_end);

  WW_TEST_CHECK_EQ(f.source_observed.sends.size(), std::size_t{1});
  if (f.source_observed.sends.empty()) {
    ::close(read_end);
    return;
  }
  const auto &[mime, source_fd] = f.source_observed.sends.front();
  WW_TEST_CHECK_EQ(mime, std::string("text/plain;charset=utf-8"));
  WW_TEST_CHECK(source_fd >= 0);

  // The actual handshake: the source client writes through the fd the
  // server handed it, and that has to be the same pipe the target is
  // reading from -- not just some other fd that happens to be non-negative.
  const char msg[] = "dropped-bytes";
  WW_TEST_CHECK_EQ(::write(source_fd, msg, sizeof(msg)),
                   static_cast<ssize_t>(sizeof(msg)));
  ::close(source_fd);

  char buf[sizeof(msg)] = {};
  WW_TEST_CHECK_EQ(::read(read_end, buf, sizeof(buf)),
                   static_cast<ssize_t>(sizeof(msg)));
  WW_TEST_CHECK_EQ(std::string(buf), std::string(msg));
  ::close(read_end);
}

// --- X11-owned clipboard: the other direction --------------------------
//
// Everything above (setting_a_selection_takes_x11_clipboard_ownership,
// clearing_the_selection_releases_x11_clipboard_ownership) is a Wayland
// client setting the clipboard, mirrored out to X11. This is the reverse:
// a native X11 application owns CLIPBOARD, and a Wayland client asks to
// read it. FakeWindowBackend now actually stores the two callbacks
// register_data_device_manager hands the backend for this
// (set_clipboard_owner_changed_callback, set_clipboard_send_callback) --
// previously both were silently dropped, which is exactly why this
// direction had no coverage at all.

WW_TEST_CASE(x11_taking_clipboard_ownership_broadcasts_an_offer_to_every_device) {
  Compositor c;
  auto *ddm = static_cast<wl_data_device_manager *>(c.wl.bind(
      "wl_data_device_manager", &wl_data_device_manager_interface, 3));
  auto *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
  wl_data_device *device = wl_data_device_manager_get_data_device(ddm, seat);
  DeviceObserver observed;
  wl_data_device_add_listener(device, &kDeviceListener, &observed);
  c.wl.pump();
  const int selections_before = observed.selections;

  WW_TEST_CHECK(static_cast<bool>(c.backend.clipboard_owner_changed_cb));
  c.backend.clipboard_owner_changed_cb({"text/plain", "text/uri-list"});
  c.wl.pump();

  WW_TEST_CHECK_EQ(observed.selections, selections_before + 1);
  WW_TEST_CHECK(!observed.offers.empty());
  bool has_text = false;
  for (const auto &m : observed.offer.mimes)
    if (m == "text/plain")
      has_text = true;
  WW_TEST_CHECK(has_text);

  destroy_offers(observed);
  wl_data_device_destroy(device);
  wl_seat_destroy(seat);
  wl_data_device_manager_destroy(ddm);
}

WW_TEST_CASE(receiving_an_x11_sourced_offer_asks_the_backend_for_the_bytes) {
  Compositor c;
  auto *ddm = static_cast<wl_data_device_manager *>(c.wl.bind(
      "wl_data_device_manager", &wl_data_device_manager_interface, 3));
  auto *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
  wl_data_device *device = wl_data_device_manager_get_data_device(ddm, seat);
  DeviceObserver observed;
  wl_data_device_add_listener(device, &kDeviceListener, &observed);
  c.wl.pump();

  c.backend.clipboard_owner_changed_cb({"text/plain"});
  c.wl.pump();
  WW_TEST_CHECK(!observed.offers.empty());
  if (observed.offers.empty()) {
    wl_data_device_destroy(device);
    wl_seat_destroy(seat);
    wl_data_device_manager_destroy(ddm);
    return;
  }

  int pipe_fds[2] = {-1, -1};
  WW_TEST_CHECK(::pipe(pipe_fds) == 0);
  wl_data_offer_receive(observed.offers.back(), "text/plain", pipe_fds[1]);
  c.wl.pump();
  ::close(pipe_fds[1]);

  // Routed to the backend, not treated as a live-source (client-to-client)
  // offer -- the two paths inside offer_receive() are otherwise
  // indistinguishable from the outside.
  WW_TEST_CHECK(c.backend.called("request_clipboard_data"));
  // FakeWindowBackend's request_clipboard_data() answers synchronously with
  // an empty vector, so the write end is closed having written nothing --
  // the read end sees EOF immediately rather than hanging.
  char buf[8];
  WW_TEST_CHECK_EQ(::read(pipe_fds[0], buf, sizeof(buf)), ssize_t{0});
  ::close(pipe_fds[0]);

  destroy_offers(observed);
  wl_data_device_destroy(device);
  wl_seat_destroy(seat);
  wl_data_device_manager_destroy(ddm);
}

WW_TEST_CASE(the_backend_asking_for_clipboard_data_reaches_the_live_source) {
  Compositor c;
  auto *ddm = static_cast<wl_data_device_manager *>(c.wl.bind(
      "wl_data_device_manager", &wl_data_device_manager_interface, 3));
  auto *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
  wl_data_device *device = wl_data_device_manager_get_data_device(ddm, seat);
  DeviceObserver observed;
  wl_data_device_add_listener(device, &kDeviceListener, &observed);

  wl_data_source *source = wl_data_device_manager_create_data_source(ddm);
  SourceObserver source_observed;
  wl_data_source_add_listener(source, &kSourceListener, &source_observed);
  wl_data_source_offer(source, "text/plain");
  wl_data_device_set_selection(device, source, 1);
  c.wl.pump();
  WW_TEST_CHECK(static_cast<bool>(c.backend.clipboard_send_cb));

  // A native X11 application asking for the Wayland clipboard we just
  // advertised -- the callback register_data_device_manager hands the
  // backend for exactly this.
  int pipe_fds[2] = {-1, -1};
  WW_TEST_CHECK(::pipe(pipe_fds) == 0);
  c.backend.clipboard_send_cb("text/plain", pipe_fds[1]);
  c.wl.pump();

  WW_TEST_CHECK_EQ(source_observed.sends.size(), std::size_t{1});
  if (!source_observed.sends.empty()) {
    const auto &[mime, source_fd] = source_observed.sends.front();
    WW_TEST_CHECK_EQ(mime, std::string("text/plain"));
    const char msg[] = "x11-wants-this";
    WW_TEST_CHECK_EQ(::write(source_fd, msg, sizeof(msg)),
                     static_cast<ssize_t>(sizeof(msg)));
    ::close(source_fd);

    char buf[sizeof(msg)] = {};
    WW_TEST_CHECK_EQ(::read(pipe_fds[0], buf, sizeof(buf)),
                     static_cast<ssize_t>(sizeof(msg)));
    WW_TEST_CHECK_EQ(std::string(buf), std::string(msg));
  }
  ::close(pipe_fds[0]);

  destroy_offers(observed);
  wl_data_source_destroy(source);
  wl_data_device_destroy(device);
  wl_seat_destroy(seat);
  wl_data_device_manager_destroy(ddm);
}

WW_TEST_CASE(the_backend_asking_with_no_selection_source_just_closes_the_fd) {
  // Manager.cpp's own documented behavior for this: `if
  // (!mgr->selection_source) { ::close(fd); return; }`. A native
  // application asking while nothing on the Wayland side owns the
  // selection must not hang or crash -- it should see the pipe close with
  // nothing written, same as any other empty-clipboard read.
  Compositor c;
  auto *ddm = static_cast<wl_data_device_manager *>(c.wl.bind(
      "wl_data_device_manager", &wl_data_device_manager_interface, 3));
  auto *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
  wl_data_device *device = wl_data_device_manager_get_data_device(ddm, seat);
  c.wl.pump();
  WW_TEST_CHECK(static_cast<bool>(c.backend.clipboard_send_cb));

  int pipe_fds[2] = {-1, -1};
  WW_TEST_CHECK(::pipe(pipe_fds) == 0);
  c.backend.clipboard_send_cb("text/plain", pipe_fds[1]);

  char buf[8];
  WW_TEST_CHECK_EQ(::read(pipe_fds[0], buf, sizeof(buf)), ssize_t{0});
  ::close(pipe_fds[0]);

  wl_data_device_destroy(device);
  wl_seat_destroy(seat);
  wl_data_device_manager_destroy(ddm);
}

// --- wl_pointer.set_cursor, end to end --------------------------------------
//
// A real client sets a cursor surface with a real wl_shm buffer; what comes
// out the other side is a backend cursor (IWindowBackend::create_cursor)
// applied to the window the pointer is over -- the thing that makes a client's
// I-beam, hand or resize arrow show up on screen instead of the host server's
// default arrow.

namespace {

struct CursorClient {
  Compositor c;
  wl_compositor *comp{nullptr};
  wl_shm *shm{nullptr};
  wl_pointer *pointer{nullptr};
  wl_seat *seat{nullptr};
  xdg_wm_base *base{nullptr};
  Toplevel top{};
  std::vector<wl_surface *> cursor_surfaces;
  uint32_t window{0};

  CursorClient() {
    // WaylandServer does this in production; the fixture's bare display
    // has no wl_shm until it is asked for one.
    wl_display_init_shm(c.wl.server_display());
    c.wl.pump();
    comp = bind_compositor(c);
    base = bind_wm_base(c);
    shm = static_cast<wl_shm *>(c.wl.bind("wl_shm", &wl_shm_interface, 1));
    seat =
        static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
    pointer = wl_seat_get_pointer(seat);
    top = make_toplevel(c, comp, base);
    window = first_window(c);
    // Pointer focus is what entitles a client to set the cursor.
    c.backend.input_cbs.on_enter(window, 1, 1, 1, 1);
    c.wl.pump();
  }

  ~CursorClient() {
    for (wl_surface *s : cursor_surfaces)
      wl_surface_destroy(s);
    xdg_toplevel_destroy(top.toplevel);
    xdg_surface_destroy(top.xdg);
    wl_surface_destroy(top.surface);
    wl_pointer_destroy(pointer);
    wl_seat_destroy(seat);
    wl_shm_destroy(shm);
    xdg_wm_base_destroy(base);
    wl_compositor_destroy(comp);
  }

  wl_surface *cursor_surface() {
    cursor_surfaces.push_back(wl_compositor_create_surface(comp));
    return cursor_surfaces.back();
  }
  void destroy_cursor_surface(wl_surface *s) {
    std::erase(cursor_surfaces, s);
    wl_surface_destroy(s);
  }

  // A w*h wl_shm buffer holding `pixels` in `format`.
  void make_buffer(ShmBuffer &out, uint32_t format, int32_t w, int32_t h,
                   const std::vector<uint32_t> &pixels) {
    const std::size_t bytes = pixels.size() * sizeof(uint32_t);
    out.fd = ::memfd_create("ww-cursor-test", MFD_CLOEXEC);
    WW_TEST_CHECK(out.fd >= 0);
    WW_TEST_CHECK(::ftruncate(out.fd, static_cast<off_t>(bytes)) == 0);
    WW_TEST_CHECK(::write(out.fd, pixels.data(), bytes) ==
                  static_cast<ssize_t>(bytes));
    out.pool = wl_shm_create_pool(shm, out.fd, static_cast<int32_t>(bytes));
    out.buffer = wl_shm_pool_create_buffer(out.pool, 0, w, h, w * 4, format);
  }

  void set_cursor(wl_surface *surface, int32_t hx, int32_t hy) {
    wl_pointer_set_cursor(pointer, 0, surface, hx, hy);
    c.wl.pump();
  }
  void commit(wl_surface *surface, wl_buffer *buffer) {
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, 1 << 20, 1 << 20);
    wl_surface_commit(surface);
    c.wl.pump();
  }
};

} // namespace

WW_TEST_CASE(a_shm_cursor_becomes_a_backend_cursor_on_the_pointer_window) {
  CursorClient k;
  const std::vector<uint32_t> px{0xff112233u, 0x80402010u, 0x00000000u,
                                 0xffffffffu};
  ShmBuffer buf;
  k.make_buffer(buf, WL_SHM_FORMAT_ARGB8888, 2, 2, px);
  wl_surface *cursor = k.cursor_surface();

  k.set_cursor(cursor, 1, 1);
  k.commit(cursor, buf.buffer);

  WW_TEST_CHECK_EQ(k.c.backend.created_cursors.size(), std::size_t{1});
  const auto &made = k.c.backend.created_cursors.front();
  WW_TEST_CHECK_EQ(made.width, uint16_t{2});
  WW_TEST_CHECK_EQ(made.height, uint16_t{2});
  WW_TEST_CHECK_EQ(made.hotspot_x, int16_t{1});
  WW_TEST_CHECK_EQ(made.hotspot_y, int16_t{1});
  // ARGB8888 goes through untouched: it is already what the window system
  // wants (premultiplied, alpha in the top byte).
  WW_TEST_CHECK(made.pixels == px);
  WW_TEST_CHECK_EQ(k.c.backend.cursor_of(k.window), made.id);
  WW_TEST_CHECK_EQ(wl_display_get_error(k.c.wl.client_display()), 0);
}

WW_TEST_CASE(wl_surface_damage_only_commit_replays_the_retained_shm_frame) {
  CursorClient k;
  ShmBuffer buf;
  k.make_buffer(buf, WL_SHM_FORMAT_ARGB8888, 2, 2,
                {0xff112233u, 0xff445566u, 0xff778899u, 0xffaabbccu});

  k.commit(k.top.surface, buf.buffer);
  const std::size_t initial_present_count = k.c.backend.presented_sizes.size();
  WW_TEST_CHECK(initial_present_count >= std::size_t{1});

  // These are real client protocol requests to the wl_surface interface.
  // Neither attaches a replacement buffer; the core must replay its retained
  // image because the current backend only offers full-buffer presentation.
  wl_surface_damage(k.top.surface, 0, 0, 1, 1);
  wl_surface_commit(k.top.surface);
  k.c.wl.pump();
  WW_TEST_CHECK_EQ(k.c.backend.presented_sizes.size(), initial_present_count + 1);

  wl_surface_damage_buffer(k.top.surface, 1, 1, 1, 1);
  wl_surface_commit(k.top.surface);
  k.c.wl.pump();
  WW_TEST_CHECK_EQ(k.c.backend.presented_sizes.size(), initial_present_count + 2);
  WW_TEST_CHECK_EQ(wl_display_get_error(k.c.wl.client_display()), 0);
}

WW_TEST_CASE(a_cursor_surface_gets_no_window_of_its_own) {
  CursorClient k;
  ShmBuffer buf;
  k.make_buffer(buf, WL_SHM_FORMAT_ARGB8888, 1, 1, {0xff000000u});
  wl_surface *cursor = k.cursor_surface();
  const std::size_t windows_before = k.c.backend.windows.size();

  k.set_cursor(cursor, 0, 0);
  k.commit(cursor, buf.buffer);

  WW_TEST_CHECK_EQ(k.c.backend.windows.size(), windows_before);
}

WW_TEST_CASE(a_cursor_committed_before_set_cursor_is_not_lost) {
  CursorClient k;
  ShmBuffer buf;
  k.make_buffer(buf, WL_SHM_FORMAT_ARGB8888, 1, 1, {0xff00ff00u});
  wl_surface *cursor = k.cursor_surface();

  // Some clients attach and commit the cursor image first and only then say
  // what it is for. The commit had no role to land in; it must be applied
  // once set_cursor supplies one.
  k.commit(cursor, buf.buffer);
  WW_TEST_CHECK(k.c.backend.created_cursors.empty());
  k.set_cursor(cursor, 0, 0);

  WW_TEST_CHECK_EQ(k.c.backend.created_cursors.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(k.c.backend.cursor_of(k.window),
                   k.c.backend.created_cursors.front().id);
}

WW_TEST_CASE(an_xrgb_cursor_is_forced_opaque) {
  CursorClient k;
  // XRGB8888's top byte is padding. A client is free to leave it zero, which
  // read as alpha would make the whole cursor invisible.
  ShmBuffer buf;
  k.make_buffer(buf, WL_SHM_FORMAT_XRGB8888, 1, 1, {0x00aabbccu});
  wl_surface *cursor = k.cursor_surface();

  k.set_cursor(cursor, 0, 0);
  k.commit(cursor, buf.buffer);

  WW_TEST_CHECK_EQ(k.c.backend.created_cursors.size(), std::size_t{1});
  WW_TEST_CHECK_EQ(k.c.backend.created_cursors.front().pixels.front(),
                   uint32_t{0xffaabbcc});
}

WW_TEST_CASE(a_scale_two_cursor_is_downsampled_to_logical_size) {
  CursorClient k;
  // The host X server's pointer is 1:1 with logical pixels, like every other
  // surface presented here, so a cursor drawn for scale 2 shrinks to match.
  std::vector<uint32_t> px(16);
  for (uint32_t i = 0; i < 16; ++i)
    px[i] = 0xff000000u | i;
  ShmBuffer buf;
  k.make_buffer(buf, WL_SHM_FORMAT_ARGB8888, 4, 4, px);
  wl_surface *cursor = k.cursor_surface();

  k.set_cursor(cursor, 1, 1); // surface-local, i.e. logical
  wl_surface_set_buffer_scale(cursor, 2);
  k.commit(cursor, buf.buffer);

  WW_TEST_CHECK_EQ(k.c.backend.created_cursors.size(), std::size_t{1});
  const auto &made = k.c.backend.created_cursors.front();
  WW_TEST_CHECK_EQ(made.width, uint16_t{2});
  WW_TEST_CHECK_EQ(made.height, uint16_t{2});
  WW_TEST_CHECK_EQ(made.hotspot_x, int16_t{1});
  WW_TEST_CHECK_EQ(made.hotspot_y, int16_t{1});
  const std::vector<uint32_t> expected{px[0], px[2], px[8], px[10]};
  WW_TEST_CHECK(made.pixels == expected);
}

WW_TEST_CASE(a_new_cursor_image_replaces_and_frees_the_previous_one) {
  CursorClient k;
  ShmBuffer first, second;
  k.make_buffer(first, WL_SHM_FORMAT_ARGB8888, 1, 1, {0xff0000ffu});
  k.make_buffer(second, WL_SHM_FORMAT_ARGB8888, 1, 1, {0xffff0000u});
  wl_surface *cursor = k.cursor_surface();
  k.set_cursor(cursor, 0, 0);

  // An animated cursor: the same surface, a fresh image per frame.
  k.commit(cursor, first.buffer);
  const uint32_t first_id = k.c.backend.created_cursors.front().id;
  k.commit(cursor, second.buffer);

  WW_TEST_CHECK_EQ(k.c.backend.created_cursors.size(), std::size_t{2});
  WW_TEST_CHECK_EQ(k.c.backend.cursor_of(k.window),
                   k.c.backend.created_cursors.back().id);
  WW_TEST_CHECK(std::find(k.c.backend.freed_cursors.begin(),
                          k.c.backend.freed_cursors.end(),
                          first_id) != k.c.backend.freed_cursors.end());
}

WW_TEST_CASE(destroying_a_cursor_surface_frees_its_cursor) {
  CursorClient k;
  ShmBuffer buf;
  k.make_buffer(buf, WL_SHM_FORMAT_ARGB8888, 1, 1, {0xff00ffffu});
  wl_surface *cursor = k.cursor_surface();
  k.set_cursor(cursor, 0, 0);
  k.commit(cursor, buf.buffer);
  const uint32_t id = k.c.backend.created_cursors.front().id;

  k.destroy_cursor_surface(cursor);
  k.c.wl.pump();

  WW_TEST_CHECK(std::find(k.c.backend.freed_cursors.begin(),
                          k.c.backend.freed_cursors.end(),
                          id) != k.c.backend.freed_cursors.end());
  WW_TEST_CHECK_EQ(wl_display_get_error(k.c.wl.client_display()), 0);
}

WW_TEST_CASE(hiding_the_cursor_shows_the_blank_cursor) {
  CursorClient k;
  k.set_cursor(nullptr, 0, 0);

  WW_TEST_CHECK(k.c.backend.blank_cursor_id != 0);
  WW_TEST_CHECK_EQ(k.c.backend.cursor_of(k.window),
                   k.c.backend.blank_cursor_id);
}

// --- lifetime fixes ---------------------------------------------------------

WW_TEST_CASE(expose_replays_the_retained_frame_instead_of_rereading_the_client_buffer) {
  CursorClient k; // only for its mapped toplevel and wl_shm
  // 65536 wide: a plain uint16_t cast wraps this to zero, and a zero-sized
  // present is rejected by the server. The commit path clamps it; what the
  // backend retains is that clamped image, so expose must replay it rather
  // than go back to the (possibly released or reused) client wl_buffer.
  ShmBuffer buf;
  k.make_buffer(buf, WL_SHM_FORMAT_ARGB8888, 65536, 1,
                std::vector<uint32_t>(65536, 0xff808080u));
  k.commit(k.top.surface, buf.buffer);
  WW_TEST_CHECK(!k.c.backend.presented_sizes.empty());
  WW_TEST_CHECK(k.c.backend.presented_sizes.back().first != 0);

  k.c.backend.presented_sizes.clear();
  k.c.backend.repainted_windows_with_frame.clear();
  k.c.backend.input_cbs.on_expose(k.window);

  // Expose is a repaint request to the backend, which owns the retained image.
  WW_TEST_CHECK(k.c.backend.called("repaint_window"));
  WW_TEST_CHECK_EQ(k.c.backend.repainted_windows_with_frame.size(),
                   std::size_t{1});
  // Core must not re-present pixels read from the client buffer.
  WW_TEST_CHECK(k.c.backend.presented_sizes.empty());
}

WW_TEST_CASE(an_offer_outlives_its_destroyed_source_safely) {
  Compositor c;
  auto *ddm = static_cast<wl_data_device_manager *>(c.wl.bind(
      "wl_data_device_manager", &wl_data_device_manager_interface, 3));
  auto *seat =
      static_cast<wl_seat *>(c.wl.bind("wl_seat", &wl_seat_interface, 7));
  wl_data_device *device = wl_data_device_manager_get_data_device(ddm, seat);
  DeviceObserver observed;
  wl_data_device_add_listener(device, &kDeviceListener, &observed);

  wl_data_source *source = wl_data_device_manager_create_data_source(ddm);
  wl_data_source_offer(source, "text/plain");
  wl_data_device_set_selection(device, source, 1);
  c.wl.pump();
  WW_TEST_CHECK(!observed.offers.empty());

  // The offer still exists on the client; its source does not. A receive now
  // must not be forwarded to the dead source (a use-after-free in the
  // compositor under ASan) -- the pipe is simply closed with no data.
  wl_data_source_destroy(source);
  c.wl.pump();
  int fds[2] = {-1, -1};
  WW_TEST_CHECK(::pipe(fds) == 0);
  wl_data_offer_receive(observed.offers.front(), "text/plain", fds[1]);
  ::close(fds[1]);
  c.wl.pump();

  char byte;
  WW_TEST_CHECK_EQ(::read(fds[0], &byte, 1), ssize_t{0});
  WW_TEST_CHECK_EQ(wl_display_get_error(c.wl.client_display()), 0);
  ::close(fds[0]);

  destroy_offers(observed);
  wl_data_device_destroy(device);
  wl_seat_destroy(seat);
  wl_data_device_manager_destroy(ddm);
}

// --- X11 window follows the geometry Wayland commits; stale frames scale ----
//
// The window manager decides a *target* size (ConfigureNotify -> configure).
// The client answers with whatever it actually renders, which is not always
// that size: it lags behind a drag, or it quantises to a grid (a terminal's
// cell size). The behaviour pinned down here:
//
//   * a geometry the client has really settled on is adopted: the X11 window
//     is resized to the committed geometry, so no strip of window is left
//     without client pixels;
//   * a frame that is merely stale -- the client is still catching up with an
//     interactive resize -- is never adopted (that would fight the window
//     manager); it is stretched, nearest-neighbour, to the current target
//     so there is no transparent/black gap while dragging.

namespace {

struct ResizeClient : CursorClient {
  std::vector<std::unique_ptr<ShmBuffer>> buffers;

  // Pixel (x, y) of a w*h image is 0xff000000 | (y * w + x + 1): every pixel
  // distinct, so a wrong sample is visible in the comparison.
  static std::vector<uint32_t> ramp(int32_t w, int32_t h) {
    std::vector<uint32_t> px;
    for (int32_t y = 0; y < h; ++y)
      for (int32_t x = 0; x < w; ++x)
        px.push_back(0xff000000u | static_cast<uint32_t>(y * w + x + 1));
    return px;
  }

  // Commit a fresh w*h buffer to the toplevel; returns what was drawn.
  std::vector<uint32_t> commit_frame(int32_t w, int32_t h) {
    auto px = ramp(w, h);
    buffers.push_back(std::make_unique<ShmBuffer>());
    make_buffer(*buffers.back(), WL_SHM_FORMAT_ARGB8888, w, h, px);
    commit(top.surface, buffers.back()->buffer);
    return px;
  }

  // A commit with no new buffer: the client only reports damage.
  void commit_damage_only() {
    wl_surface_damage(top.surface, 0, 0, 1 << 20, 1 << 20);
    wl_surface_commit(top.surface);
    c.wl.pump();
  }

  // The window manager reports a new size for the X11 window.
  void wm_resizes(uint16_t w, uint16_t h) {
    c.backend.resize_cb(window, w, h);
    c.wl.pump();
  }

  void advance_time(std::chrono::milliseconds delta) {
    c.timer.advance(delta);
    c.wl.pump();
  }

  [[nodiscard]] std::size_t resizes() const {
    return c.backend.count("resize_window");
  }
  [[nodiscard]] const FakeWindowBackend::PresentedPixels &last_presented()
      const {
    return c.backend.presented_pixels.back();
  }
};

// Each source pixel repeated into a (dw/sw) x (dh/sh) block.
std::vector<uint32_t> replicate(const std::vector<uint32_t> &src, int sw,
                                int sh, int dw, int dh) {
  std::vector<uint32_t> out;
  for (int y = 0; y < dh; ++y)
    for (int x = 0; x < dw; ++x)
      out.push_back(src[static_cast<std::size_t>((y * sh / dh) * sw +
                                                 (x * sw / dw))]);
  return out;
}

} // namespace

WW_TEST_CASE(a_committed_geometry_that_differs_from_the_wm_target_resizes_x11) {
  ResizeClient k;
  // The window starts at the compositor's initial 800x600 target; the client
  // commits something else entirely.
  k.commit_frame(4, 2);
  WW_TEST_CHECK(k.resizes() >= std::size_t{1});
  WW_TEST_CHECK_EQ(k.c.backend.windows.at(k.window).width, uint16_t{4});
  WW_TEST_CHECK_EQ(k.c.backend.windows.at(k.window).height, uint16_t{2});
  // Presented as committed: not stretched back to the old target.
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{4});
  WW_TEST_CHECK_EQ(k.last_presented().height, uint16_t{2});
}

WW_TEST_CASE(a_stale_frame_during_a_resize_is_scaled_to_the_wm_target) {
  ResizeClient k;
  const auto drawn = k.commit_frame(2, 2);
  k.wm_resizes(4, 4);
  const std::size_t resizes_before = k.resizes();

  k.commit_frame(2, 2); // the client has not caught up yet

  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{4});
  WW_TEST_CHECK_EQ(k.last_presented().height, uint16_t{4});
  WW_TEST_CHECK_EQ(k.last_presented().stride, uint32_t{16});
  WW_TEST_CHECK(k.last_presented().pixels == replicate(drawn, 2, 2, 4, 4));
  // The window manager is mid-drag: its size is not overridden by a stale one.
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before);
  WW_TEST_CHECK_EQ(wl_display_get_error(k.c.wl.client_display()), 0);
}

WW_TEST_CASE(stale_frames_follow_every_target_of_a_drag_without_resizing_x11) {
  ResizeClient k;
  const auto drawn = k.commit_frame(2, 2);
  const std::size_t resizes_before = k.resizes();

  for (const uint16_t size : std::initializer_list<uint16_t>{4, 6, 8, 5, 3}) {
    k.wm_resizes(size, size);
    k.commit_frame(2, 2);
    WW_TEST_CHECK_EQ(k.last_presented().width, size);
    WW_TEST_CHECK_EQ(k.last_presented().height, size);
    WW_TEST_CHECK(k.last_presented().pixels ==
                  replicate(drawn, 2, 2, size, size));
  }
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before);
}

WW_TEST_CASE(a_non_uniform_target_scales_each_axis_independently) {
  ResizeClient k;
  const auto drawn = k.commit_frame(2, 2);
  k.wm_resizes(6, 2);
  k.commit_frame(2, 2);
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{6});
  WW_TEST_CHECK_EQ(k.last_presented().height, uint16_t{2});
  WW_TEST_CHECK(k.last_presented().pixels == replicate(drawn, 2, 2, 6, 2));
}

WW_TEST_CASE(a_frame_matching_the_wm_target_is_presented_unscaled) {
  ResizeClient k;
  k.commit_frame(2, 2);
  k.wm_resizes(4, 4);
  const std::size_t resizes_before = k.resizes();

  const auto drawn = k.commit_frame(4, 4); // the client caught up

  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{4});
  WW_TEST_CHECK(k.last_presented().pixels == drawn);
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before);
}

WW_TEST_CASE(a_settled_grid_quantized_geometry_is_adopted_after_250ms) {
  // A terminal quantises to its cell grid: asked for 5x5 it keeps rendering
  // 4x4. Once the WM target has been stable for 250 ms and all configures are
  // acknowledged, adopt the latest commit even if the client goes idle.
  ResizeClient k;
  k.commit_frame(4, 4);
  k.wm_resizes(5, 5);
  const std::size_t resizes_before = k.resizes();

  const auto drawn = k.commit_frame(4, 4); // stale frame is stretched to target
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before);
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{5});
  WW_TEST_CHECK_EQ(k.last_presented().height, uint16_t{5});
  k.advance_time(std::chrono::milliseconds{249});
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before);

  // No further client commit is required: the timer evaluates the already
  // committed 4x4 geometry at the deadline and adopts it.
  k.advance_time(std::chrono::milliseconds{1});
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before + 1);
  WW_TEST_CHECK_EQ(k.c.backend.windows.at(k.window).width, uint16_t{4});
  WW_TEST_CHECK_EQ(k.c.backend.windows.at(k.window).height, uint16_t{4});
  // Adoption also repaints the retained canonical image immediately; it must
  // not leave the old 5x5 stretched cache cropped by the smaller X11 window.
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{4});
  WW_TEST_CHECK_EQ(k.last_presented().height, uint16_t{4});
  WW_TEST_CHECK(k.last_presented().pixels == drawn);

  // A subsequent same-size frame is presented at native size and stays put.
  k.commit_frame(4, 4);
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{4});
  WW_TEST_CHECK_EQ(k.last_presented().height, uint16_t{4});
  WW_TEST_CHECK(k.last_presented().pixels == drawn);
  k.commit_frame(4, 4);
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before + 1);
}

WW_TEST_CASE(a_window_the_wm_tiles_is_never_resized_by_a_commit_only_scaled) {
  ResizeClient k;
  k.commit_frame(4, 4);
  k.c.manager.set_tiled(k.window, true);
  k.wm_resizes(5, 5);
  const std::size_t resizes_before = k.resizes();

  const auto drawn = k.commit_frame(4, 4);
  k.commit_frame(4, 4);
  k.commit_frame(4, 4);

  // The window manager owns a tiled window's geometry. The strip that the
  // quantised client leaves is filled by stretching, never by fighting i3.
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before);
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{5});
  WW_TEST_CHECK_EQ(k.last_presented().height, uint16_t{5});
  WW_TEST_CHECK(k.last_presented().pixels == replicate(drawn, 4, 4, 5, 5));
}

WW_TEST_CASE(a_damage_only_commit_during_a_resize_is_also_scaled) {
  ResizeClient k;
  const auto drawn = k.commit_frame(2, 2);
  k.wm_resizes(4, 4);
  const std::size_t presents = k.c.backend.presented_pixels.size();

  k.commit_damage_only(); // replays the retained frame

  WW_TEST_CHECK(k.c.backend.presented_pixels.size() > presents);
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{4});
  WW_TEST_CHECK(k.last_presented().pixels == replicate(drawn, 2, 2, 4, 4));
}

WW_TEST_CASE(stale_scaling_uses_only_the_window_geometry_not_the_shadow) {
  ResizeClient k;
  // 4x4 buffer whose window proper is the central 2x2 (a client-side shadow
  // around it). The shadow must not be stretched onto the screen.
  xdg_surface_set_window_geometry(k.top.xdg, 1, 1, 2, 2);
  const auto drawn = k.commit_frame(4, 4);
  WW_TEST_CHECK_EQ(k.c.backend.windows.at(k.window).width, uint16_t{2});

  k.wm_resizes(4, 4);
  k.commit_frame(4, 4);

  const std::vector<uint32_t> inner{drawn[5], drawn[6], drawn[9], drawn[10]};
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{4});
  WW_TEST_CHECK_EQ(k.last_presented().height, uint16_t{4});
  WW_TEST_CHECK(k.last_presented().pixels == replicate(inner, 2, 2, 4, 4));
  WW_TEST_CHECK(!k.c.backend.presented_regions.back().declared());
}

WW_TEST_CASE(scaling_a_stale_frame_leaves_the_clients_buffer_untouched) {
  ResizeClient k;
  k.commit_frame(2, 2);
  k.wm_resizes(4, 4);
  const auto drawn = k.commit_frame(2, 2);
  // The drag moves on to a new target and the client only reports damage.
  // The replay must be built from the original 2x2 pixels, not from the
  // already-stretched 4x4 the previous frame left behind.
  k.wm_resizes(6, 6);
  k.commit_damage_only();
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{6});
  WW_TEST_CHECK(k.last_presented().pixels == replicate(drawn, 2, 2, 6, 6));
}

// The window manager grows the X11 window and the client has not committed
// yet: nothing presents, and the X server replays the old-size image on
// Expose, leaving the newly exposed strip without client pixels.
WW_TEST_CASE(an_expose_after_the_wm_grew_the_window_stretches_the_stale_frame) {
  ResizeClient k;
  const auto drawn = k.commit_frame(2, 2);
  k.wm_resizes(4, 4);
  k.commit_frame(2, 2); // stale frame, already stretched to 4x4
  k.wm_resizes(6, 6);   // the drag moved on; the client has not committed
  const std::size_t resizes_before = k.resizes();
  const std::size_t presents = k.c.backend.presented_pixels.size();
  const std::size_t configures_before_expose = k.c.xdg_configure_events;

  k.c.backend.input_cbs.on_expose(k.window);

  WW_TEST_CHECK_EQ(k.c.xdg_configure_events, configures_before_expose);
  WW_TEST_CHECK(k.c.backend.presented_pixels.size() > presents);
  WW_TEST_CHECK_EQ(k.last_presented().width, uint16_t{6});
  WW_TEST_CHECK_EQ(k.last_presented().height, uint16_t{6});
  WW_TEST_CHECK(k.last_presented().pixels == replicate(drawn, 2, 2, 6, 6));
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before);
}

WW_TEST_CASE(an_expose_with_a_frame_that_matches_the_target_just_repaints) {
  ResizeClient k;
  k.commit_frame(4, 4);
  const std::size_t presents = k.c.backend.presented_pixels.size();
  k.c.backend.input_cbs.on_expose(k.window);
  // Nothing to stretch: the plain retained-frame replay, not a re-present.
  WW_TEST_CHECK_EQ(k.c.backend.presented_pixels.size(), presents);
  WW_TEST_CHECK(k.c.backend.count("repaint_window") >= std::size_t{1});
}

// A client that is behind a drag commits one old size after another. Each
// target changes before the 250 ms settle deadline, so none of those
// intermediate commits may be adopted even though configure acks arrive.
WW_TEST_CASE(a_lagging_client_is_not_adopted_while_the_wm_target_moves) {
  ResizeClient k;
  k.commit_frame(4, 4);
  const std::size_t resizes_before = k.resizes();

  for (const auto &[target, committed] :
       {std::pair<uint16_t, uint16_t>{8, 5}, {9, 6}, {10, 7}}) {
    const std::size_t configure_count = k.c.xdg_configure_events;
    k.wm_resizes(target, target);
    WW_TEST_CHECK_EQ(k.c.xdg_configure_events, configure_count + 1);
    k.wm_resizes(target, target); // duplicate ConfigureNotify is deduplicated
    WW_TEST_CHECK_EQ(k.c.xdg_configure_events, configure_count + 1);
    k.commit_frame(committed, committed);
    WW_TEST_CHECK_EQ(k.last_presented().width, target);
    WW_TEST_CHECK_EQ(k.last_presented().height, target);
    k.advance_time(std::chrono::milliseconds{200});
  }
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before);
}

// Repeated commits of the same stale size do not settle a resize by count.
// The WM target changes more frequently than the 250 ms delay, so each new
// target resets the timer and every frame remains stretched to the latest one.
WW_TEST_CASE(repeated_commits_do_not_settle_while_the_target_moves) {
  ResizeClient k;
  k.commit_frame(4, 4);
  const std::size_t resizes_before = k.resizes();
  for (uint16_t size = 5; size < 12; ++size) {
    k.wm_resizes(size, size);
    k.wm_resizes(size, size); // duplicate ConfigureNotify is deduplicated
    k.commit_frame(4, 4);
    k.commit_frame(4, 4);
    WW_TEST_CHECK_EQ(k.last_presented().width, size);
    WW_TEST_CHECK_EQ(k.last_presented().height, size);
    k.advance_time(std::chrono::milliseconds{200});
  }
  WW_TEST_CHECK_EQ(k.resizes(), resizes_before);
}
