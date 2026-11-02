// wl_subcompositor / wl_subsurface — see impl.hpp for how this
// fits with the other files in this directory.
#include "wwindows/adapters/wayland/globals/core/detail/impl.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wwindows/support/log.hpp>

#include <cstdint>

namespace ww {

namespace {
// Carries both the manager and the owning wl_surface resource so the
// wl_subsurface request handlers below can hand off to SurfaceManager
// without a separate lookup table. Only used within this file, unlike
// RegionData, so it stays local rather than moving to the shared header.
struct SubsurfaceData {
  SurfaceManager *mgr;
  wl_resource *surface;
};

void subsurface_destroy(wl_client *, wl_resource *r) {
  wl_resource_destroy(r);
}

void subsurface_set_position(wl_client *, wl_resource *r, int32_t x,
                             int32_t y) {
  auto *d = static_cast<SubsurfaceData *>(wl_resource_get_user_data(r));
  if (d && d->mgr)
    d->mgr->set_subsurface_position(d->surface, x, y);
}

void subsurface_place_above(wl_client *, wl_resource *r,
                            wl_resource *sibling) {
  auto *d = static_cast<SubsurfaceData *>(wl_resource_get_user_data(r));
  if (d && d->mgr)
    d->mgr->place_subsurface(d->surface, sibling, /*above=*/true);
}

void subsurface_place_below(wl_client *, wl_resource *r,
                            wl_resource *sibling) {
  auto *d = static_cast<SubsurfaceData *>(wl_resource_get_user_data(r));
  if (d && d->mgr)
    d->mgr->place_subsurface(d->surface, sibling, /*above=*/false);
}

void subsurface_set_sync(wl_client *, wl_resource *r) {
  auto *d = static_cast<SubsurfaceData *>(wl_resource_get_user_data(r));
  if (d && d->mgr)
    d->mgr->set_subsurface_sync(d->surface, /*sync=*/true);
}

void subsurface_set_desync(wl_client *, wl_resource *r) {
  auto *d = static_cast<SubsurfaceData *>(wl_resource_get_user_data(r));
  if (d && d->mgr)
    d->mgr->set_subsurface_sync(d->surface, /*sync=*/false);
}

const struct wl_subsurface_interface subsurface_iface = {
    .destroy = subsurface_destroy,
    .set_position = subsurface_set_position,
    .place_above = subsurface_place_above,
    .place_below = subsurface_place_below,
    .set_sync = subsurface_set_sync,
    .set_desync = subsurface_set_desync,
};

void subsurface_resource_destroy(wl_resource *r) {
  auto *d = static_cast<SubsurfaceData *>(wl_resource_get_user_data(r));
  if (d) {
    WW_LOG_TRACE("[wayland] wl_subsurface destroy (id={})",
                wl_resource_get_id(r));
    if (d->mgr)
      d->mgr->destroy_subsurface_role(d->surface);
    delete d;
  }
}

void subcompositor_destroy(wl_client *, wl_resource *r) {
  wl_resource_destroy(r);
}

void subcompositor_get_subsurface(wl_client *client, wl_resource *resource,
                                  uint32_t id, wl_resource *surface,
                                  wl_resource *parent) {
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  WW_LOG_DEBUG("[wayland] subcompositor.get_subsurface (id={}, surface={}, "
              "parent={})",
              id, wl_resource_get_id(surface), wl_resource_get_id(parent));
  wl_resource *sub = wl_resource_create(
      client, &wl_subsurface_interface, wl_resource_get_version(resource), id);
  auto *data = new SubsurfaceData{mgr, surface};
  wl_resource_set_implementation(sub, &subsurface_iface, data,
                                 subsurface_resource_destroy);
  if (mgr)
    mgr->create_subsurface(sub, surface, parent);
}

const struct wl_subcompositor_interface subcompositor_iface = {
    .destroy = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};
} // namespace

void bind_subcompositor(wl_client *client, void *data, uint32_t version,
                        uint32_t id) {
  wl_resource *r = wl_resource_create(client, &wl_subcompositor_interface,
                                      static_cast<int>(version), id);
  wl_resource_set_implementation(r, &subcompositor_iface, data, nullptr);
}

} // namespace ww
