#include "wwindows/adapters/wayland/globals/xdg_decoration.hpp"
#include <wwindows/support/log.hpp>

#include <wwindows/adapters/wayland/protocols/zxdg-decoration-unstable-v1-server-protocol.h>
#include <wayland-server-core.h>

namespace ww {

// -------------------------------------------------------------------------
// zxdg_toplevel_decoration_v1
// -------------------------------------------------------------------------

static void toplevel_decoration_destroy(wl_client *, wl_resource *r) {
  wl_resource_destroy(r);
}

static void toplevel_decoration_set_mode(wl_client *, wl_resource *r,
                                         uint32_t /*mode*/) {
  zxdg_toplevel_decoration_v1_send_configure(
      r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void toplevel_decoration_unset_mode(wl_client *, wl_resource *r) {
  zxdg_toplevel_decoration_v1_send_configure(
      r, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static const struct zxdg_toplevel_decoration_v1_interface
    toplevel_decoration_impl = {
        .destroy = toplevel_decoration_destroy,
        .set_mode = toplevel_decoration_set_mode,
        .unset_mode = toplevel_decoration_unset_mode,
};

static void decoration_manager_destroy(wl_client *, wl_resource *r) {
  wl_resource_destroy(r);
}

static void
decoration_manager_get_toplevel_decoration(wl_client *client,
                                           wl_resource *resource, uint32_t id,
                                           wl_resource * /*toplevel*/) {
  wl_resource *dec =
      wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface,
                         wl_resource_get_version(resource), id);
  wl_resource_set_implementation(dec, &toplevel_decoration_impl, nullptr,
                                 nullptr);

  zxdg_toplevel_decoration_v1_send_configure(
      dec, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  WW_LOG_DEBUG("[wayland] toplevel decoration created (forced server-side)");
}

static const struct zxdg_decoration_manager_v1_interface
    decoration_manager_impl = {
        .destroy = decoration_manager_destroy,
        .get_toplevel_decoration = decoration_manager_get_toplevel_decoration,
};

static void bind_decoration_manager(wl_client *client, void * /*data*/,
                                    uint32_t version, uint32_t id) {
  // Support up to version 2 (version 2 allows get_toplevel_decoration on
  // toplevels that already have buffers attached).
  const int max_version = 2;
  int bind_version = static_cast<int>(std::min<uint32_t>(version, max_version));
  wl_resource *r = wl_resource_create(
      client, &zxdg_decoration_manager_v1_interface, bind_version, id);
  wl_resource_set_implementation(r, &decoration_manager_impl, nullptr, nullptr);
}

VoidResult register_xdg_decoration(wl_display *display) {
  // Advertise version 2 to allow clients to use get_toplevel_decoration on
  // toplevels with attached buffers (a v1 protocol error).
  if (!wl_global_create(display, &zxdg_decoration_manager_v1_interface, 2,
                        nullptr, bind_decoration_manager))
    return std::unexpected(Error::wayland_init(
        "Failed to create zxdg_decoration_manager_v1 global"));

  WW_LOG_INFO("[wayland] xdg-decoration v2 registered (always server-side)");
  return {};
}

} // namespace ww