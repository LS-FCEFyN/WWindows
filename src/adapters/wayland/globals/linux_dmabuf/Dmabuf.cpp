// zwp_linux_dmabuf_v1 — the linux-dmabuf global; creates
// zwp_linux_buffer_params_v1 objects (Params.cpp), and owns
// register_linux_dmabuf(), the public entry point declared in
// LinuxDmabuf.hpp. See impl.hpp for how this fits with the other files in
// this directory.
#include "wwindows/adapters/wayland/globals/linux_dmabuf.hpp"
#include "wwindows/adapters/wayland/globals/linux_dmabuf/detail/impl.hpp"
#include "wwindows/ports/window_backend.hpp"
#include <wwindows/support/log.hpp>

#include <cstdint>
#include <wwindows/adapters/wayland/protocols/linux-dmabuf-unstable-v1-server-protocol.h>
#include <wayland-server-core.h>

namespace ww {

namespace {
void dmabuf_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

// get_default_feedback/get_surface_feedback are defined in Feedback.cpp
// (declared in impl.hpp) — this vtable just wires them in, same as
// create_params below.
const struct zwp_linux_dmabuf_v1_interface dmabuf_iface = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_get_default_feedback,
    .get_surface_feedback = dmabuf_get_surface_feedback,
};

void bind_linux_dmabuf(wl_client *client, void *data, uint32_t version,
                       uint32_t id) {
  wl_resource *r =
      wl_resource_create(client, &zwp_linux_dmabuf_v1_interface,
                         static_cast<int>(version), id);
  wl_resource_set_implementation(r, &dmabuf_iface, data, nullptr);

  // Only advertise formats do_create() can actually present correctly —
  // see Params.cpp's comment on why ABGR8888 is excluded. Feedback.cpp's
  // format table is built from this same SUPPORTED_DMABUF_FORMATS list, so
  // the two can't disagree.
  for (uint32_t fmt : SUPPORTED_DMABUF_FORMATS) {
    zwp_linux_dmabuf_v1_send_format(r, fmt);
    if (version >= DMABUF_MODIFIER_SINCE)
      zwp_linux_dmabuf_v1_send_modifier(r, fmt,
                                        /*modifier_hi=*/0, /*modifier_lo=*/0);
  }
}
} // namespace

VoidResult register_linux_dmabuf(wl_display *display, IWindowBackend &backend) {
  // A dmabuf global backed by no device can't import anything — do_create()
  // would accept the request and then every buffer would silently never
  // present (import_dmabuf() returning 0 is the only signal, and nothing
  // downstream currently surfaces that as a client-visible error). Match
  // what the README documents: don't advertise the global at all when
  // there's nothing behind it, and let clients fall back to wl_shm, which
  // they're required to support unconditionally.
  if (!backend.dmabuf_main_device()) {
    WW_LOG_INFO("[wayland] linux-dmabuf not registered — no dmabuf-capable "
                "device available (DRI3/Present unavailable); clients will "
                "use wl_shm instead");
    return {};
  }

  // v4 for get_default_feedback/get_surface_feedback (Feedback.cpp).
  if (!wl_global_create(display, &zwp_linux_dmabuf_v1_interface, 4, &backend,
                        bind_linux_dmabuf)) {
    return std::unexpected(
        Error::wayland_init("Failed to create zwp_linux_dmabuf_v1 global"));
  }

  WW_LOG_INFO(
      "[wayland] linux-dmabuf v4 registered (ARGB8888, XRGB8888, MOD_LINEAR)");
  return {};
}

} // namespace ww
