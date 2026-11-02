// xdg_wm_base — the xdg-shell global; creates xdg_positioner
// (Positioner.cpp) and xdg_surface (Surface.cpp) objects, and owns
// register_xdg_shell(), the public entry point declared in XdgShell.hpp.
// See impl.hpp for how this fits with the other files in this directory.
#include "wwindows/adapters/wayland/globals/xdg_shell.hpp"
#include "wwindows/adapters/wayland/globals/xdg_shell/detail/impl.hpp"
#include "wwindows/core/surface_manager.hpp"
#include <wwindows/adapters/wayland/protocols/xdg-shell-server-protocol.h>
#include <wayland-server-core.h>
#include <wwindows/support/log.hpp>

#include <cstdint>

namespace ww {

namespace {
void wm_base_destroy(struct wl_client *, struct wl_resource *resource) {
  wl_resource_destroy(resource);
}
void wm_base_create_positioner(struct wl_client *client,
                               struct wl_resource *resource, uint32_t id) {
  create_positioner(client, wl_resource_get_version(resource), id);
}
void wm_base_get_xdg_surface(struct wl_client *client,
                             struct wl_resource *resource, uint32_t id,
                             struct wl_resource *surface) {
  auto surface_manager =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  // xdg-shell: creating a second xdg_surface for a wl_surface that already
  // has one (or any other role) is a protocol error, not silently
  // tolerated -- the second xdg_surface would otherwise become an
  // independent object racing the first one's set_toplevel/get_popup calls
  // against the very same underlying Surface.
  if (surface_manager && surface_manager->has_xdg_surface_role(surface)) {
    wl_resource_post_error(resource, XDG_WM_BASE_ERROR_ROLE,
                           "wl_surface@%u already has an xdg_surface",
                           wl_resource_get_id(surface));
    return;
  }
  WW_LOG_DEBUG("[wayland] xdg_wm_base.get_xdg_surface (xdg_surface_id={}, surface={})",
               id, wl_resource_get_id(surface));
  struct wl_resource *xdg_surface = wl_resource_create(
      client, &xdg_surface_interface, wl_resource_get_version(resource), id);

  auto data = new XdgSurfaceData{surface_manager, surface};
  wl_resource_set_implementation(xdg_surface, &xdg_surface_impl, data,
                                 xdg_surface_resource_destroy);
  if (surface_manager)
    surface_manager->mark_xdg_surface_role(surface);
}
void wm_base_pong(struct wl_client *, struct wl_resource *resource,
                  uint32_t serial) {
  WW_LOG_TRACE("[wayland] xdg_wm_base.pong (wm_base={}, serial={})",
               wl_resource_get_id(resource), serial);
}

const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = wm_base_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

void bind_xdg_wm_base(struct wl_client *client, void *data, uint32_t version,
                      uint32_t id) {
  struct wl_resource *resource =
      wl_resource_create(client, &xdg_wm_base_interface, static_cast<int>(version), id);
  wl_resource_set_implementation(resource, &wm_base_impl, data, nullptr);
}
} // namespace

VoidResult register_xdg_shell(wl_display *display,
                              SurfaceManager &surface_manager) {
  if (!wl_global_create(display, &xdg_wm_base_interface, 3, &surface_manager,
                        bind_xdg_wm_base))
    return std::unexpected(
        Error::wayland_init("Failed to create xdg_wm_base global"));

  WW_LOG_INFO("[wayland] xdg-shell registered");
  return {};
}

} // namespace ww
