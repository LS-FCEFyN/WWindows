#include "wwindows/adapters/wayland/wayland_server.hpp"
#include "wwindows/adapters/wayland/globals/core_globals.hpp"
#include "wwindows/adapters/wayland/globals/fractional_scale.hpp"
#include "wwindows/adapters/wayland/globals/xdg_decoration.hpp"
#include "wwindows/adapters/wayland/globals/xdg_shell.hpp"
#include "wwindows/adapters/wayland/globals/viewporter.hpp"
#include "wwindows/adapters/wayland/globals/foreign_toplevel_list.hpp"
#include "wwindows/adapters/wayland/globals/image_capture_source.hpp"
#include "wwindows/adapters/wayland/globals/image_copy_capture.hpp"

#include "wwindows/core/event_loop.hpp"
#include "wwindows/input/input_translator.hpp"
#include <wwindows/support/log.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace ww {

// Logs every request/event that crosses the wire. This is real protocol
// traffic, not a one-off debugging aid, so it stays in the codebase — but
// it's opt-in via WW_PROTOCOL_LOG=1 (and gated to trace level besides)
// because it fires on every single client request.
static void protocol_log(void *, enum wl_protocol_logger_type direction,
          const struct wl_protocol_logger_message *message) {
  const char *dir = (direction == WL_PROTOCOL_LOGGER_REQUEST) ? "req" : "evt";
  const char *iface = message->resource && message->resource->object.interface
           ? message->resource->object.interface->name
           : "<null>";
  WW_LOG_TRACE("[wayland-proto] {} {}.{} (obj={})", dir, iface,
    message->message ? message->message->name : "<null>",
    message->resource ? message->resource->object.id : 0);
}

struct WlDisplayDeleter {
  void operator()(wl_display *d) const noexcept {
    if (!d)
      return;
    // wl_display_destroy() does not tear down still-connected clients, so
    // every resource they own -- and every destroy listener attached to one
    // -- would simply leak. Reaping clients first both frees them and lets
    // those listeners run while the objects observing them (SurfaceManager,
    // InputTranslator) are still alive, which the destruction order in
    // src/application.cpp is arranged to guarantee.
    wl_display_destroy_clients(d);
    wl_display_destroy(d);
  }
};
using WlDisplayPtr = std::unique_ptr<wl_display, WlDisplayDeleter>;

static VoidResult ensure_runtime_dir() {
  const char *dir = ::getenv("XDG_RUNTIME_DIR");
  if (dir && dir[0] != '\0')
    return {};

  std::string fallback = "/tmp/wwindows-runtime-" + std::to_string(::getuid());
  if (::mkdir(fallback.c_str(), 0700) < 0 && errno != EEXIST)
    return std::unexpected(
        Error::wayland_socket("cannot create fallback XDG_RUNTIME_DIR " +
                              fallback + ": " + ::strerror(errno)));

  ::setenv("XDG_RUNTIME_DIR", fallback.c_str(), 0);
  WW_LOG_WARN("[wayland] XDG_RUNTIME_DIR not set, using fallback {}", fallback);
  return {};
}

struct WaylandServer::Impl {
  WlDisplayPtr display;
  std::string socket_name;
  wl_event_loop *wl_loop{nullptr};
  // Owned by the display. Kept so a screen change can be pushed to clients.
  OutputGlobal *output{nullptr};
  FractionalScaleGlobal *fractional_scale{nullptr};
};

WaylandServer::WaylandServer() : impl_{std::make_unique<Impl>()} {}

WaylandServer::~WaylandServer() { WW_LOG_INFO("[wayland] server destroyed"); }

void WaylandServer::update_output(const OutputState &state) {
  if (impl_->output)
    ww::update_output(*impl_->output, state);
  if (impl_->fractional_scale)
    update_fractional_scale(
        *impl_->fractional_scale,
        compute_scale_120ths(state.width_px, state.width_mm));
}

Result<std::unique_ptr<WaylandServer>>
WaylandServer::create(EventLoop &loop, SurfaceManager &surface_manager,
                      InputTranslator &input_translator,
                      const OutputState &output_state) {
  if (auto r = ensure_runtime_dir(); !r)
    return std::unexpected(r.error());

  auto server = std::unique_ptr<WaylandServer>(new WaylandServer());
  auto &im = *server->impl_;

  wl_display *raw = wl_display_create();
  if (!raw) {
    WW_LOG_ERROR("[wayland] wl_display_create failed");
    return std::unexpected(Error::wayland_init("wl_display_create failed"));
  }
  im.display = WlDisplayPtr{raw};

  const char *name = wl_display_add_socket_auto(raw);
  if (!name) {
    WW_LOG_ERROR("[wayland] wl_display_add_socket_auto failed: {}",
                 ::strerror(errno));
    return std::unexpected(Error::wayland_socket(
        std::string{"wl_display_add_socket_auto failed: "} +
        ::strerror(errno)));
  }
  im.socket_name = name;

  if (std::getenv("WW_PROTOCOL_LOG")) {
    wl_display_add_protocol_logger(raw, protocol_log, nullptr);
    WW_LOG_INFO("[wayland] protocol logging enabled (WW_PROTOCOL_LOG set)");
  }

  // ── Core globals (wl_compositor, wl_seat, wl_output) ─────────────────
  if (auto r = register_core_globals(raw, surface_manager, input_translator,
                                     output_state);
      !r) {
    WW_LOG_ERROR("[wayland] core globals registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  } else {
    im.output = *r;
  }

  // ── XDG shell (xdg_wm_base) ───────────────────────────────────────────
  if (auto r = register_xdg_shell(raw, surface_manager); !r) {
    WW_LOG_ERROR("[wayland] xdg-shell registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  }

  // ── XDG decoration manager ────────────────────────────────────────────
  if (auto r = register_xdg_decoration(raw); !r) {
    WW_LOG_ERROR("[wayland] xdg-decoration registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  }

  // ── Fractional scale ──────────────────────────────────────────────────
  const uint32_t scale_120ths =
      compute_scale_120ths(output_state.width_px, output_state.width_mm);
  if (auto r = register_fractional_scale(raw, scale_120ths); !r) {
    WW_LOG_ERROR("[wayland] fractional-scale registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  } else {
    im.fractional_scale = *r;
  }

  // ── Viewporter (wp_viewport for fractional scaling) ─────────────────────
  if (auto r = register_viewporter(raw); !r) {
    WW_LOG_ERROR("[wayland] viewporter registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  }

  // ── Foreign toplevel list (ext_foreign_toplevel_list_v1) ───────────────
  if (auto r = ww::foreign_toplevel_list::register_foreign_toplevel_list(raw, surface_manager); !r) {
    WW_LOG_ERROR("[wayland] foreign-toplevel-list registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  }

  // ── Image capture source (ext_image_capture_source_v1) ─────────────────
  if (auto r = ww::image_capture_source::register_image_capture_source(raw, surface_manager); !r) {
    WW_LOG_ERROR("[wayland] image-capture-source registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  }

  // ── Image copy capture (ext_image_copy_capture_v1) ─────────────────────
  if (auto r = ww::image_copy_capture::register_image_copy_capture(raw, surface_manager); !r) {
    WW_LOG_ERROR("[wayland] image-copy-capture registration failed: {}",
                 r.error().message);
    return std::unexpected(r.error());
  }

  // ── Shared memory (wl_shm) ────────────────────────────────────────────
  wl_display_init_shm(raw);

  // ── Integrate Wayland event loop with our poll-based event loop ───────
  im.wl_loop = wl_display_get_event_loop(raw);
  int wl_fd = wl_event_loop_get_fd(im.wl_loop);

  auto result = loop.add(wl_fd, IoIn | IoErr,
                         [raw, wl_loop = im.wl_loop, &loop](uint32_t events) {
                           if (events & IoErr) {
                             WW_LOG_INFO("[wayland] display error, shutting down");
                             loop.stop(0);
                             return;
                           }
                           if (events & IoIn) {
                             wl_event_loop_dispatch(wl_loop, 0);
                             wl_display_flush_clients(raw);
                           }
                         });
  if (!result) {
    WW_LOG_ERROR("[wayland] failed to register wl fd with event loop: {}",
                 result.error().message);
    return std::unexpected(result.error());
  }

  ::setenv("WAYLAND_DISPLAY", name, 1);
  WW_LOG_INFO("[wayland] listening on {} (fd={})", im.socket_name, wl_fd);
  return server;
}

wl_display *WaylandServer::display() const { return impl_->display.get(); }
std::string_view WaylandServer::socket_name() const {
  return impl_->socket_name;
}

} // namespace ww
