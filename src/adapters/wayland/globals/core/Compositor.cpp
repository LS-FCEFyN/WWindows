// wl_compositor — creates wl_surface (Surface.cpp) and wl_region
// (Region.cpp) objects. See impl.hpp for how this fits with the other
// files in this directory.
#include "wwindows/adapters/wayland/globals/core/detail/impl.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wwindows/support/log.hpp>

#include <cstdint>

namespace ww {

namespace {
void compositor_create_surface(wl_client *client, wl_resource *resource,
                               uint32_t id) {
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  WW_LOG_DEBUG("[wayland] compositor.create_surface (id={})", id);
  wl_resource *surf = wl_resource_create(client, &wl_surface_interface,
                                         wl_resource_get_version(resource), id);
  wl_resource_set_implementation(surf, &surface_iface, mgr,
                                 surface_resource_destroy);
  if (mgr)
    mgr->create_surface(surf);
}

void compositor_create_region(wl_client *client, wl_resource *, uint32_t id) {
  wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
  wl_resource_set_implementation(region, &region_iface, new RegionData{},
                                 region_resource_destroy);
}

const struct wl_compositor_interface compositor_iface = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
#ifdef WWINDOWS_HAVE_WL_COMPOSITOR_RELEASE
    // .release was added to wl_compositor_interface as of protocol
    // version 7 by newer libwayland (detected at configure time). The
    // wl_compositor global below is advertised at version 5, so a client
    // can never actually send this request — nullptr is unreachable, not
    // just a warning silencer.
    .release = nullptr,
#endif
};
} // namespace

void bind_compositor(wl_client *client, void *data, uint32_t version,
                     uint32_t id) {
  wl_resource *r =
      wl_resource_create(client, &wl_compositor_interface,
                          static_cast<int>(version), id);
  wl_resource_set_implementation(r, &compositor_iface, data, nullptr);
}

} // namespace ww
