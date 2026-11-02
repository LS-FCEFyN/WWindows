// wl_surface — see impl.hpp for how this fits with the other files in
// this directory.
#include "wwindows/adapters/wayland/globals/core/detail/impl.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wwindows/support/log.hpp>

#include <cstdint>

namespace ww {

namespace {
void surface_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }

void surface_attach(wl_client *, wl_resource *resource, wl_resource *buffer,
                    int32_t x, int32_t y) {
  // wl_surface is always created at version 5 in this compositor (see
  // register_core_globals capping the version), and the spec makes a
  // non-zero x/y here a protocol error at that version: wl_surface.offset
  // is the only correct way to say this now. A client sending both would
  // be relying on behavior version 5 explicitly forbids, so this is
  // caught here rather than silently ignored like it used to be.
  if (x != 0 || y != 0) {
    wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_OFFSET,
                           "wl_surface.attach: non-zero x/y is not allowed "
                           "at this surface version; use wl_surface.offset "
                           "instead");
    return;
  }
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  WW_LOG_TRACE("[wayland] surface.attach (surface={}, buffer={})",
               wl_resource_get_id(resource),
               buffer ? wl_resource_get_id(buffer) : 0);
  if (mgr)
    mgr->attach_buffer(resource, buffer);
}

void surface_damage(wl_client *, wl_resource *resource, int32_t, int32_t,
                    int32_t width, int32_t height) {
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  if (mgr)
    mgr->mark_surface_damage(resource, width, height);
}

void surface_frame(wl_client *client, wl_resource *resource, uint32_t id) {
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  WW_LOG_TRACE("[wayland] surface.frame (surface={}, callback_id={})",
               wl_resource_get_id(resource), id);
  wl_resource *cb = wl_resource_create(client, &wl_callback_interface, 1, id);
  if (mgr) {
    mgr->add_frame_callback(resource, cb);
  } else {
    wl_callback_send_done(cb, 0);
    wl_resource_destroy(cb);
  }
}

void surface_set_opaque_region(wl_client *, wl_resource *, wl_resource *) {}
void surface_set_input_region(wl_client *, wl_resource *resource,
                              wl_resource *region) {
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  if (!mgr)
    return;

  if (!region) {
    // set_input_region(nullptr) resets to "whole surface accepts input".
    mgr->set_pending_input_region(resource, {}, {}, /*has_region=*/false);
    return;
  }

  auto *d = static_cast<RegionData *>(wl_resource_get_user_data(region));
  if (!d)
    return;
  // Copy now: the client may destroy or keep mutating the region object
  // after this call returns, and set_input_region is a snapshot.
  mgr->set_pending_input_region(resource, d->add_rects, d->sub_rects,
                                /*has_region=*/true);
}

void surface_commit(wl_client *, wl_resource *resource) {
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  WW_LOG_TRACE("[wayland] surface.commit (surface={})",
               wl_resource_get_id(resource));
  if (mgr)
    mgr->commit(resource);
}

void surface_set_buffer_transform(wl_client *, wl_resource *, int32_t) {}
void surface_set_buffer_scale(wl_client *, wl_resource *resource,
                              int32_t scale) {
  if (scale < 1) {
    wl_resource_post_error(resource, WL_SURFACE_ERROR_INVALID_SCALE,
                           "wl_surface.set_buffer_scale: scale must be "
                           "positive, got %d", scale);
    return;
  }
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  if (mgr)
    mgr->set_pending_buffer_scale(resource, scale);
}
void surface_damage_buffer(wl_client *, wl_resource *resource, int32_t,
                           int32_t, int32_t width, int32_t height) {
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  if (mgr)
    mgr->mark_surface_damage(resource, width, height);
}
void surface_offset(wl_client *, wl_resource *resource, int32_t x,
                    int32_t y) {
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  WW_LOG_TRACE("[wayland] surface.offset ({}, {}) (surface={})", x, y,
               wl_resource_get_id(resource));
  if (mgr)
    mgr->set_pending_offset(resource, x, y);
}
} // namespace

const struct wl_surface_interface surface_iface = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
#ifdef WWINDOWS_HAVE_WL_SURFACE_GET_RELEASE
    // .get_release was added to wl_surface_interface by newer libwayland
    // (detected at configure time; not present on every distro's older
    // libwayland, hence the #ifdef). Every wl_surface is created with the
    // version the client bound on wl_compositor (see
    // compositor_create_surface in Compositor.cpp), which is
    // capped at 5 by register_core_globals — below whatever version
    // introduced get_release — so a client can never send this request.
    // nullptr is unreachable, not just a warning silencer.
    .get_release = nullptr,
#endif
};

void surface_resource_destroy(wl_resource *resource) {
  auto *mgr =
      static_cast<SurfaceManager *>(wl_resource_get_user_data(resource));
  WW_LOG_TRACE("[wayland] wl_surface destroy (surface={})",
               wl_resource_get_id(resource));
  if (mgr)
    mgr->destroy_surface(resource);
}

} // namespace ww
