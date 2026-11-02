// xdg_toplevel — see impl.hpp for how this fits with the other files in
// this directory.
#include "wwindows/adapters/wayland/globals/xdg_shell/detail/impl.hpp"
#include "wwindows/core/surface_manager.hpp"
#include <wwindows/adapters/wayland/protocols/xdg-shell-server-protocol.h>
#include <wayland-server-core.h>

#include <cstdint>

namespace ww {

namespace {
void toplevel_destroy(struct wl_client *, struct wl_resource *resource) {
  wl_resource_destroy(resource);
}
void toplevel_set_parent(struct wl_client *, struct wl_resource *resource,
                         struct wl_resource *parent) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (!data || !data->manager)
    return;
  // The parent is another xdg_toplevel, whose user data is the same
  // XdgSurfaceData as its xdg_surface's (see xdg_surface_get_toplevel).
  auto parent_data =
      parent ? static_cast<XdgSurfaceData *>(wl_resource_get_user_data(parent))
             : nullptr;
  data->manager->set_toplevel_parent(data->surface,
                                     parent_data ? parent_data->surface : nullptr);
}
void toplevel_set_title(struct wl_client *, struct wl_resource *resource,
                        const char *title) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->manager && title) {
    data->manager->set_title(data->surface, title);
  }
}
void toplevel_set_app_id(struct wl_client *, struct wl_resource *resource,
                         const char *app_id) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->manager)
    data->manager->set_app_id(data->surface, app_id);
}
// show_window_menu has no EWMH/ICCCM equivalent: there is no
// widely-implemented, spec-defined way for an X11 client to ask a window
// manager to pop up a title-bar-style menu the way there is for move,
// resize, iconify, maximize and fullscreen. The closest real-world
// convention, _GTK_SHOW_WINDOW_MENU, is a GTK/Mutter-originated
// ClientMessage that KWin also honors, but it is not part of any
// published spec this project can verify the exact field layout of
// against a primary source (unlike _NET_WM_MOVERESIZE and
// WM_CHANGE_STATE below, both taken directly from EWMH/ICCCM text); an
// implementation guessed from secondhand descriptions risks silently
// sending a malformed request that is worse than today's no-op. Left
// unimplemented until a verifiable spec is in hand. i3 -- the primary
// window manager this project is developed against -- has no window
// menu concept at all regardless, so this is a no-op there either way.
void toplevel_show_window_menu(struct wl_client *, struct wl_resource *,
                               struct wl_resource *, uint32_t, int32_t,
                               int32_t) {}
void toplevel_move(struct wl_client *, struct wl_resource *resource,
                   struct wl_resource *, uint32_t) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->manager)
    data->manager->move_toplevel(data->surface);
}
void toplevel_resize(struct wl_client *, struct wl_resource *resource,
                     struct wl_resource *, uint32_t, uint32_t edges) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->manager)
    data->manager->resize_toplevel(data->surface, edges);
}
void toplevel_set_max_size(struct wl_client *, struct wl_resource *resource,
                           int32_t width, int32_t height) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->manager)
    data->manager->set_max_size(data->surface, width, height);
}
void toplevel_set_min_size(struct wl_client *, struct wl_resource *resource,
                           int32_t width, int32_t height) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->manager)
    data->manager->set_min_size(data->surface, width, height);
}
// set_maximized / set_fullscreen and their unset_ counterparts all reduce
// to the same thing here: WWindows does not own top-level geometry, the X11
// window manager does, so forward the request and let the resulting size
// come back through the ordinary resize path.
//
// Fullscreen is the one that matters most. A window manager hands out its
// *work area* — the screen minus panels and bars — and that is the ceiling
// on every ordinary toplevel; asking for fullscreen is the only way a
// client gets the whole screen.
void toplevel_request_state(struct wl_resource *resource, ww::WindowState st,
                            bool enabled) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->manager)
    data->manager->set_toplevel_state(data->surface, st, enabled);
}
void toplevel_set_maximized(struct wl_client *, struct wl_resource *r) {
  toplevel_request_state(r, ww::WindowState::Maximized, true);
}
void toplevel_unset_maximized(struct wl_client *, struct wl_resource *r) {
  toplevel_request_state(r, ww::WindowState::Maximized, false);
}
void toplevel_set_fullscreen(struct wl_client *, struct wl_resource *r,
                             struct wl_resource *) {
  toplevel_request_state(r, ww::WindowState::Fullscreen, true);
}
void toplevel_unset_fullscreen(struct wl_client *, struct wl_resource *r) {
  toplevel_request_state(r, ww::WindowState::Fullscreen, false);
}
void toplevel_set_minimized(struct wl_client *, struct wl_resource *resource) {
  auto data =
      static_cast<XdgSurfaceData *>(wl_resource_get_user_data(resource));
  if (data && data->manager)
    data->manager->minimize_toplevel(data->surface);
}
} // namespace

const struct xdg_toplevel_interface toplevel_interface = {
    .destroy = toplevel_destroy,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_title,
    .set_app_id = toplevel_set_app_id,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_set_max_size,
    .set_min_size = toplevel_set_min_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_set_minimized,
};

} // namespace ww
