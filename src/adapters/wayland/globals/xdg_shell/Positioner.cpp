// xdg_positioner — see impl.hpp for how this fits with the other files in
// this directory.
#include "wwindows/adapters/wayland/globals/xdg_shell/detail/impl.hpp"
#include <wwindows/adapters/wayland/protocols/xdg-shell-server-protocol.h>
#include <wayland-server-core.h>

#include <cstdint>

namespace ww {

void compute_popup_position(const PositionerState &pos, int32_t &out_x,
                            int32_t &out_y) {
  int32_t ax;
  int32_t ay;
  switch (pos.anchor) {
  case XDG_POSITIONER_ANCHOR_TOP:
    ax = pos.anchor_x + pos.anchor_w / 2;
    ay = pos.anchor_y;
    break;
  case XDG_POSITIONER_ANCHOR_BOTTOM:
    ax = pos.anchor_x + pos.anchor_w / 2;
    ay = pos.anchor_y + pos.anchor_h;
    break;
  case XDG_POSITIONER_ANCHOR_LEFT:
    ax = pos.anchor_x;
    ay = pos.anchor_y + pos.anchor_h / 2;
    break;
  case XDG_POSITIONER_ANCHOR_RIGHT:
    ax = pos.anchor_x + pos.anchor_w;
    ay = pos.anchor_y + pos.anchor_h / 2;
    break;
  case XDG_POSITIONER_ANCHOR_TOP_LEFT:
    ax = pos.anchor_x;
    ay = pos.anchor_y;
    break;
  case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
    ax = pos.anchor_x + pos.anchor_w;
    ay = pos.anchor_y;
    break;
  case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
    ax = pos.anchor_x;
    ay = pos.anchor_y + pos.anchor_h;
    break;
  case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
    ax = pos.anchor_x + pos.anchor_w;
    ay = pos.anchor_y + pos.anchor_h;
    break;
  default: // XDG_POSITIONER_ANCHOR_NONE — center of the anchor rect
    ax = pos.anchor_x + pos.anchor_w / 2;
    ay = pos.anchor_y + pos.anchor_h / 2;
    break;
  }

  int32_t px = ax;
  int32_t py = ay;
  switch (pos.gravity) {
  case XDG_POSITIONER_GRAVITY_TOP:
    px -= pos.width / 2;
    py -= pos.height;
    break;
  case XDG_POSITIONER_GRAVITY_BOTTOM:
    px -= pos.width / 2;
    break;
  case XDG_POSITIONER_GRAVITY_LEFT:
    px -= pos.width;
    py -= pos.height / 2;
    break;
  case XDG_POSITIONER_GRAVITY_RIGHT:
    py -= pos.height / 2;
    break;
  case XDG_POSITIONER_GRAVITY_TOP_LEFT:
    px -= pos.width;
    py -= pos.height;
    break;
  case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
    py -= pos.height;
    break;
  case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
    px -= pos.width;
    break;
  case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
    break;
  default: // XDG_POSITIONER_GRAVITY_NONE — center the popup on the point
    px -= pos.width / 2;
    py -= pos.height / 2;
    break;
  }

  out_x = px + pos.offset_x;
  out_y = py + pos.offset_y;
}

void create_positioner(wl_client *client, int version, uint32_t id) {
  struct wl_resource *positioner =
      wl_resource_create(client, &xdg_positioner_interface, version, id);

  static const struct xdg_positioner_interface positioner_impl = {
      .destroy = [](struct wl_client *,
                    struct wl_resource *r) { wl_resource_destroy(r); },
      .set_size =
          [](struct wl_client *, struct wl_resource *r, int32_t w, int32_t h) {
            auto *p = static_cast<PositionerState *>(wl_resource_get_user_data(r));
            if (p) {
              p->width = w;
              p->height = h;
            }
          },
      .set_anchor_rect =
          [](struct wl_client *, struct wl_resource *r, int32_t x, int32_t y,
             int32_t w, int32_t h) {
            auto *p = static_cast<PositionerState *>(wl_resource_get_user_data(r));
            if (p) {
              p->anchor_x = x;
              p->anchor_y = y;
              p->anchor_w = w;
              p->anchor_h = h;
            }
          },
      .set_anchor =
          [](struct wl_client *, struct wl_resource *r, uint32_t anchor) {
            auto *p = static_cast<PositionerState *>(wl_resource_get_user_data(r));
            if (p)
              p->anchor = anchor;
          },
      .set_gravity =
          [](struct wl_client *, struct wl_resource *r, uint32_t gravity) {
            auto *p = static_cast<PositionerState *>(wl_resource_get_user_data(r));
            if (p)
              p->gravity = gravity;
          },
      .set_constraint_adjustment =
          [](struct wl_client *, struct wl_resource *r, uint32_t adj) {
            auto *p = static_cast<PositionerState *>(wl_resource_get_user_data(r));
            if (p)
              p->constraint_adjustment = adj;
          },
      .set_offset =
          [](struct wl_client *, struct wl_resource *r, int32_t x, int32_t y) {
            auto *p = static_cast<PositionerState *>(wl_resource_get_user_data(r));
            if (p) {
              p->offset_x = x;
              p->offset_y = y;
            }
          },
      // set_reactive / set_parent_size / set_parent_configure all feed the
      // constraint re-adjustment machinery we don't implement (see
      // compute_popup_position's comment) — accepted and ignored rather
      // than left unset, so a client that calls them doesn't get killed
      // for using a protocol-valid request.
      .set_reactive = [](struct wl_client *, struct wl_resource *) {},
      .set_parent_size = [](struct wl_client *, struct wl_resource *, int32_t,
                            int32_t) {},
      .set_parent_configure = [](struct wl_client *, struct wl_resource *,
                                 uint32_t) {}};
  wl_resource_set_implementation(
      positioner, &positioner_impl, new PositionerState{},
      [](struct wl_resource *r) {
        delete static_cast<PositionerState *>(wl_resource_get_user_data(r));
      });
}

} // namespace ww
