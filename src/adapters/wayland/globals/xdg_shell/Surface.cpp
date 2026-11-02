// xdg_surface — creates xdg_toplevel (Toplevel.cpp) and xdg_popup
// (Popup.cpp) objects. See impl.hpp for how this fits with the other files
// in this directory.
#include "wwindows/adapters/wayland/globals/xdg_shell/detail/impl.hpp"
#include "wwindows/core/surface_manager.hpp"
#include <wwindows/adapters/wayland/protocols/xdg-shell-server-protocol.h>
#include <wayland-server-core.h>
#include <wwindows/support/log.hpp>

#include <algorithm>
#include <cstdint>

namespace ww {

namespace {
void xdg_surface_destroy(struct wl_client *, struct wl_resource *resource) {
  // Just destroy the resource; the freeing happens in
  // xdg_surface_resource_destroy below, which libwayland invokes however the
  // resource dies. Deleting here instead leaked the data on every path that
  // is not an explicit client destroy request -- a client disconnecting, or
  // display teardown, neither of which sends one.
  wl_resource_destroy(resource);
}
void xdg_surface_get_toplevel(struct wl_client *client,
                              struct wl_resource *resource, uint32_t id) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->has_role) {
    wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                           "xdg_surface@%u already has a role",
                           wl_resource_get_id(resource));
    return;
  }
  WW_LOG_DEBUG("[wayland] xdg_surface.get_toplevel (xdg_surface={}, toplevel_id={})",
               wl_resource_get_id(resource), id);
  struct wl_resource *toplevel = wl_resource_create(
      client, &xdg_toplevel_interface, wl_resource_get_version(resource), id);
  wl_resource_set_implementation(toplevel, &toplevel_interface, data,
                                 nullptr);

  if (data) {
    data->has_role = true;
    if (data->manager)
      data->manager->set_toplevel(data->surface, toplevel, resource);
  }
}
void xdg_surface_get_popup(struct wl_client *client,
                           struct wl_resource *resource, uint32_t id,
                           struct wl_resource *parent_res,
                           struct wl_resource *positioner_res) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->has_role) {
    wl_resource_post_error(resource, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                           "xdg_surface@%u already has a role",
                           wl_resource_get_id(resource));
    return;
  }
  WW_LOG_DEBUG("[wayland] xdg_surface.get_popup (xdg_surface={}, popup_id={})",
               wl_resource_get_id(resource), id);

  struct wl_resource *popup = wl_resource_create(
      client, &xdg_popup_interface, wl_resource_get_version(resource), id);

  if (!data || !data->manager) {
    // No SurfaceManager to hand off to (shouldn't happen in practice) —
    // still give the client a valid, if inert, object rather than nothing.
    auto *empty = new PopupData{nullptr, nullptr, nullptr, nullptr};
    wl_resource_set_implementation(popup, &popup_interface, empty,
                                   popup_resource_destroy);
    return;
  }
  data->has_role = true;

  auto *parent_data = parent_res ? static_cast<XdgSurfaceData *>(
                                       wl_resource_get_user_data(parent_res))
                                 : nullptr;
  auto *pos = positioner_res
                  ? static_cast<PositionerState *>(
                        wl_resource_get_user_data(positioner_res))
                  : nullptr;

  int32_t x = 0;
  int32_t y = 0;
  uint16_t w = 1;
  uint16_t h = 1;
  if (pos) {
    compute_popup_position(*pos, x, y);
    w = static_cast<uint16_t>(std::max(1, pos->width));
    h = static_cast<uint16_t>(std::max(1, pos->height));
  } else {
    WW_LOG_WARN("[wayland] xdg_surface.get_popup: no positioner supplied — "
               "popup will be 1x1 at (0,0)");
  }

  auto *popup_data =
      new PopupData{data->manager, data->surface, resource,
                    parent_data ? parent_data->surface : nullptr};
  wl_resource_set_implementation(popup, &popup_interface, popup_data,
                                 popup_resource_destroy);

  if (parent_data) {
    data->manager->create_popup(data->surface, parent_data->surface, popup, x,
                                y, w, h);
  } else {
    WW_LOG_WARN(
        "[wayland] xdg_surface.get_popup: no parent xdg_surface supplied");
  }

  xdg_popup_send_configure(popup, x, y, w, h);
  const uint32_t serial =
      wl_display_next_serial(wl_client_get_display(client));
  xdg_surface_send_configure(resource, serial);
  data->manager->record_configure_serial(data->surface, serial);
}
void xdg_surface_set_window_geometry(struct wl_client *,
                                     struct wl_resource *resource, int32_t x,
                                     int32_t y, int32_t width,
                                     int32_t height) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  WW_LOG_TRACE("[wayland] xdg_surface.set_window_geometry (xdg_surface={}, "
               "{}x{}+{}+{})",
               wl_resource_get_id(resource), width, height, x, y);
  if (data && data->manager)
    data->manager->set_window_geometry(data->surface, x, y, width, height);
}
void xdg_surface_ack_configure(struct wl_client *, struct wl_resource *resource,
                               uint32_t serial) {
  auto *data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->manager &&
      !data->manager->acknowledge_configure(data->surface, serial)) {
    wl_resource_post_error(resource, XDG_SURFACE_ERROR_INVALID_SERIAL,
                           "xdg_surface.ack_configure: unknown serial %u",
                           serial);
    return;
  }
  WW_LOG_TRACE("[wayland] xdg_surface.ack_configure (xdg_surface={}, serial={})",
               wl_resource_get_id(resource), serial);
}
} // namespace

const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

// Defined outside the anonymous namespace so WmBase.cpp can install it.
void xdg_surface_resource_destroy(struct wl_resource *resource) {
  delete static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
}

} // namespace ww
