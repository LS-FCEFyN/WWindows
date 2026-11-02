// xdg_popup — see impl.hpp for how this fits with the other files in
// this directory.
#include "wwindows/adapters/wayland/globals/xdg_shell/detail/impl.hpp"
#include "wwindows/core/surface_manager.hpp"
#include <wwindows/adapters/wayland/protocols/xdg-shell-server-protocol.h>
#include <wayland-server-core.h>
#include <wwindows/support/log.hpp>

#include <algorithm>
#include <cstdint>

namespace ww {

namespace {
void popup_destroy(struct wl_client *, struct wl_resource *resource) {
  // wl_resource_destroy runs popup_resource_destroy below, which does the
  // actual SurfaceManager teardown — explicit destroy and the resource
  // destructor share one code path.
  wl_resource_destroy(resource);
}
void popup_grab(struct wl_client *, struct wl_resource *resource,
                struct wl_resource *, uint32_t) {
  // Every mapped popup already participates in
  // SurfaceManager::dismiss_popups_not_owning's click-outside stack
  // regardless of whether grab() was called, so there's nothing additional
  // to arm here — this just acknowledges the request instead of silently
  // ignoring it. A client that never calls grab() (e.g. a tooltip) gets
  // the same dismiss-on-outside-click behavior as one that does; that's a
  // known simplification (no non-grabbing popups) rather than a spec
  // violation a client can observe going wrong.
  WW_LOG_TRACE("[wayland] xdg_popup.grab (popup={})",
              wl_resource_get_id(resource));
}
void popup_reposition(struct wl_client *, struct wl_resource *resource,
                      struct wl_resource *positioner, uint32_t token) {
  auto *data = static_cast<PopupData *>(wl_resource_get_user_data(resource));
  auto *pos =
      positioner
          ? static_cast<PositionerState *>(wl_resource_get_user_data(positioner))
          : nullptr;
  if (!data || !data->manager || !pos)
    return;

  int32_t x = 0;
  int32_t y = 0;
  compute_popup_position(*pos, x, y);
  const auto w = static_cast<uint16_t>(std::max(1, pos->width));
  const auto h = static_cast<uint16_t>(std::max(1, pos->height));

  data->manager->reposition_popup(data->surface, x, y, w, h);

  if (wl_resource_get_version(resource) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION)
    xdg_popup_send_repositioned(resource, token);
  xdg_popup_send_configure(resource, x, y, w, h);

  if (data->xdg_surface) {
    wl_client *client = wl_resource_get_client(resource);
    const uint32_t serial =
        wl_display_next_serial(wl_client_get_display(client));
    xdg_surface_send_configure(data->xdg_surface, serial);
    if (data->manager)
      data->manager->record_configure_serial(data->surface, serial);
  }

  WW_LOG_DEBUG("[wayland] xdg_popup.reposition (popup={}, pos=({},{}), "
              "size={}x{}, token={})",
              wl_resource_get_id(resource), x, y, w, h, token);
}
} // namespace

const struct xdg_popup_interface popup_interface = {
    .destroy = popup_destroy,
    .grab = popup_grab,
    .reposition = popup_reposition,
};

void popup_resource_destroy(struct wl_resource *resource) {
  auto *data = static_cast<PopupData *>(wl_resource_get_user_data(resource));
  if (data && data->manager)
    data->manager->destroy_popup(data->surface);
  delete data;
}

} // namespace ww
