// wl_data_source — see impl.hpp for how this fits with the other files in
// this directory.
#include "wwindows/adapters/wayland/globals/data_device/detail/impl.hpp"
#include "wwindows/ports/window_backend.hpp"

#include <wayland-server-core.h>

#include <cstdint>

namespace ww {

namespace {
void source_offer(wl_client *, wl_resource *resource, const char *mime_type) {
  auto *src = static_cast<SourceData *>(wl_resource_get_user_data(resource));
  if (src && mime_type)
    src->mime_types.emplace_back(mime_type);
}

void source_destroy(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

void source_set_actions(wl_client *, wl_resource *resource,
                        uint32_t dnd_actions) {
  auto *src = static_cast<SourceData *>(wl_resource_get_user_data(resource));
  if (!src)
    return;
  src->dnd_actions = dnd_actions & kAllDndActions;
  src->actions_set = true;
}
} // namespace

const struct wl_data_source_interface data_source_iface = {
    .offer = source_offer,
    .destroy = source_destroy,
    .set_actions = source_set_actions,
};

void source_resource_destroy(wl_resource *r) {
  auto *src = static_cast<SourceData *>(wl_resource_get_user_data(r));
  if (!src) {
    return;
  }
  if (auto *mgr = src->mgr) {
    if (mgr->selection_source == r) {
      mgr->selection_source = nullptr;
      mgr->selection_is_x11 = false;
      if (mgr->backend)
        mgr->backend->set_clipboard_owner({});
      broadcast_selection_to_all(*mgr);
    }
    if (mgr->drag_active && mgr->drag_source == r) {
      mgr->drag_source = nullptr;
      cancel_drag(*mgr);
    }
  }
  delete src;
}

} // namespace ww
