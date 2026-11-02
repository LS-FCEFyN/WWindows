#include "wwindows/wwindows.hpp"
#include "wwindows/core/event_loop.hpp"
#include "wwindows/core/surface_manager.hpp"
#include "wwindows/ports/surface_interactions.hpp"
#include "wwindows/input/input_translator.hpp"
#include "wwindows/adapters/wayland/wayland_server.hpp"
#include "wwindows/adapters/wayland/globals/core_globals.hpp"
#include "wwindows/adapters/wayland/globals/data_device.hpp"
#include "wwindows/adapters/wayland/globals/linux_dmabuf.hpp"
#include "wwindows/adapters/i3/i3_watcher.hpp"
#include "wwindows/adapters/x11/x11_backend.hpp"

#include <wwindows/support/log.hpp>
#include <wayland-server-core.h>
#include <xcb/xcb.h>

namespace ww {

struct WWindows::Impl {
  // Declaration order = reverse destruction order.
  // Desired destruction sequence:
  //   input_translator → wayland → surface_manager → i3 → x11 → event_loop
  std::unique_ptr<EventLoop> event_loop; // destroyed last
  std::unique_ptr<X11Backend> x11;
  // Optional: only present under a window manager that has one (i3 today).
  // Holds an fd registered with event_loop, so it must go before it.
  std::unique_ptr<I3Watcher> i3;
  // Re-reads I3_SOCKET_PATH and attempts I3Watcher::connect() against
  // whatever it finds, unless `i3` is already connected. Stored here
  // (not as a WWindows::create()-local lambda) because it must keep
  // working for the lifetime of `i3` itself: both the X11 property
  // watch below and I3Watcher's own disconnected callback call it, quite
  // possibly long after create() has returned.
  std::function<void(const std::string &)> try_i3_socket;
  std::unique_ptr<SurfaceManager> surface_manager;
  std::unique_ptr<WaylandServer> wayland;
  std::unique_ptr<InputTranslator> input_translator; // destroyed first
};

WWindows::WWindows() : impl_{std::make_unique<Impl>()} {}

WWindows::~WWindows() { WW_LOG_INFO("[wwindows] shutting down"); }

Result<std::unique_ptr<WWindows>> WWindows::create() {
  WW_LOG_INFO("[wwindows] starting up");

  auto instance = std::unique_ptr<WWindows>(new WWindows());
  auto &im = *instance->impl_;

  // 1. EventLoop
  auto loop = EventLoop::create();
  if (!loop) {
    WW_LOG_ERROR("[wwindows] event loop init failed: {}", loop.error().message);
    return std::unexpected(loop.error());
  }
  im.event_loop = std::move(*loop);

  // 2. Window backend — X11Backend is the only IWindowBackend implementation
  //    today. This is the single place a different backend would be plugged
  //    in; everything downstream (SurfaceManager, InputTranslator,
  //    register_linux_dmabuf) only ever sees the IWindowBackend interface.
  auto x11 = X11Backend::create(*im.event_loop);
  if (!x11) {
    WW_LOG_ERROR("[wwindows] X11 backend init failed: {}", x11.error().message);
    return std::unexpected(x11.error());
  }
  im.x11 = std::move(*x11);

  // 3. SurfaceManager
  im.surface_manager = std::make_unique<SurfaceManager>(*im.x11, im.event_loop.get());

  // 4. InputTranslator — registers input callbacks; needs display later
  im.input_translator =
      std::make_unique<InputTranslator>(*im.x11, *im.surface_manager);

  // 5. WaylandServer — creates wl_display, registers globals
  //    The wl_output describes the X screen, which X owns: it starts from what
  //    the server reports now and is updated when RandR says it changed.
  const auto to_output_state = [](const X11Backend::ScreenGeometry &g) {
    return OutputState{.width_px = g.width_px,
                       .height_px = g.height_px,
                       .width_mm = g.width_mm,
                       .height_mm = g.height_mm,
                       .refresh_mhz = g.refresh_mhz};
  };
  const OutputState output_state = to_output_state(im.x11->screen_geometry());
  auto wayland = WaylandServer::create(*im.event_loop, *im.surface_manager,
                                       *im.input_translator, output_state);
  if (!wayland) {
    WW_LOG_ERROR("[wwindows] Wayland server init failed: {}",
                 wayland.error().message);
    return std::unexpected(wayland.error());
  }
  im.wayland = std::move(*wayland);
  im.x11->set_screen_changed_callback(
      [server = im.wayland.get(), to_output_state](
          const X11Backend::ScreenGeometry &g) {
        server->update_output(to_output_state(g));
        wl_display_flush_clients(server->display());
      });

  // Register linux-dmabuf (requires wl_display and the window backend both
  // ready)
  if (auto r = register_linux_dmabuf(im.wayland->display(), *im.x11); !r) {
    WW_LOG_ERROR("[wwindows] linux-dmabuf registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  }

  // Register wl_data_device_manager (clipboard + drag-and-drop) — needs
  // the InputTranslator to redirect pointer dispatch during a drag and to
  // learn about keyboard focus changes, and the backend to mirror the
  // Wayland selection to X11 CLIPBOARD.
  if (auto r = register_data_device_manager(im.wayland->display(),
                                            *im.surface_manager,
                                            *im.input_translator, *im.x11);
      !r) {
    WW_LOG_ERROR("[wwindows] wl_data_device_manager registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  }

  // Tell toplevels whether they hold keyboard focus (the xdg_toplevel
  // ACTIVATED state). X decides the focus; this only relays it.
  im.input_translator->set_keyboard_focus_changed_callback(
      [sm = im.surface_manager.get()](wl_resource *surface) {
        if (sm)
          sm->set_activated_surface(surface);
      });

  // 6. Finalise InputTranslator now that wl_display is available
  //    (initialises xkb, builds keymap string)
  im.input_translator->set_display(im.wayland->display());
  im.x11->set_events_processed_callback([d = im.wayland->display()]() {
    wl_display_flush_clients(d);
  });

  // Forward X11 close events to the SurfaceManager so Wayland clients are
  // told to close gracefully instead of the X11 server killing the process.
  im.x11->set_close_callback([sm = im.surface_manager.get()](uint32_t win) {
    if (sm)
      sm->request_close(win);
  });

  // Ask the window manager how it is laying our toplevels out, where it has
  // a way to say. Purely additive: with no such window manager, or if it
  // cannot be reached, toplevels are simply never told they are tiled and
  // behave as before.
  //
  // i3 publishes its IPC socket path in the I3_SOCKET_PATH property of the
  // root window; nothing else needs to be configured -- except that i3 sets
  // that property from its own independent startup sequence, and if this
  // process was itself launched from i3's config (an `exec`/`exec_always`
  // line), the two are directly racing each other. A single synchronous
  // read taken too early just comes back empty with nothing left to correct
  // it, so: try the synchronous read now for the common case (i3 was
  // already up), and also register a watch that retries if that read missed
  // or if i3 isn't running yet at all. The same watch, plus
  // I3Watcher::set_disconnected_callback below, also covers the *other*
  // problem this used to explicitly decline: reconnecting after a live
  // connection dies. i3 re-publishes I3_SOCKET_PATH as part of bringing its
  // IPC subsystem up regardless of whether restarting it means a genuinely
  // new process or the same one re-executing itself in place, so losing the
  // connection and later seeing that property change again are really the
  // same event from two directions -- both funnel into the one
  // try_i3_socket below, guarded so only one connection attempt is ever
  // in flight.
  auto *x11_ptr = im.x11.get();
  auto *sm_ptr = im.surface_manager.get();
  auto *loop_ptr = im.event_loop.get();
  auto *i3_slot = &im.i3;
  auto *impl_ptr = &im;
  im.try_i3_socket = [x11_ptr, sm_ptr, loop_ptr, i3_slot,
                     impl_ptr](const std::string &socket_path) {
    if (*i3_slot)
      return; // already connected; this is either a later, unrelated
              // property change, or the immediate retry below racing the
              // next PropertyNotify -- whichever gets here first wins,
              // and the other is then correctly a no-op.
    auto i3 = I3Watcher::connect(*loop_ptr, socket_path);
    if (!i3) {
      WW_LOG_WARN("[wwindows] i3 detected but its IPC socket is unusable, "
                  "continuing without tiling hints: {}", i3.error().message);
      return;
    }
    (*i3)->set_tiling_callback([sm_ptr](uint32_t win, ww::TiledEdges edges) {
      sm_ptr->set_tiled_edges(win, edges);
    });
    // If this connection is ever lost, clear the slot (letting a future
    // try_i3_socket call through the guard above) and immediately go
    // back to I3_SOCKET_PATH for another attempt. This can fire before
    // the new i3 has finished setting the property back up, in which
    // case root_string_property finds nothing and this particular
    // attempt is a no-op -- but the watch_root_property callback below
    // will still see that same property being set (i3 always does, as
    // part of bringing its IPC subsystem back up) and try again then.
    (*i3)->set_disconnected_callback([x11_ptr, i3_slot, impl_ptr] {
      i3_slot->reset();
      if (const auto fresh_socket_path =
              x11_ptr->root_string_property("I3_SOCKET_PATH"))
        impl_ptr->try_i3_socket(*fresh_socket_path);
    });
    *i3_slot = std::move(*i3);
  };

  if (const auto i3_socket = x11_ptr->root_string_property("I3_SOCKET_PATH")) {
    im.try_i3_socket(*i3_socket);
  } else {
    WW_LOG_INFO("[wwindows] I3_SOCKET_PATH not set (yet) -- will retry if "
                "it appears");
  }
  x11_ptr->watch_root_property(
      "I3_SOCKET_PATH", [impl_ptr](std::optional<std::string> socket_path) {
        if (socket_path)
          impl_ptr->try_i3_socket(*socket_path);
      });

  WW_LOG_INFO("[wwindows] initialised (WAYLAND_DISPLAY={})",
              im.wayland->socket_name());
  return instance;
}

int WWindows::run() {
  WW_LOG_INFO("[wwindows] running");
  const int code = impl_->event_loop->run();
  WW_LOG_INFO("[wwindows] run() returned (code={})", code);
  return code;
}

EventLoop &WWindows::event_loop() const {
  return *impl_->event_loop;
}

} // namespace ww
