// wl_data_device object itself — its vtable and lifecycle only. The two
// request bodies (start_drag, set_selection) live in Drag.cpp and
// Selection.cpp respectively, since each pulls in a whole independent
// feature rather than being a couple of lines of glue; see impl.hpp for
// the full rationale.
#include "wwindows/adapters/wayland/globals/data_device/detail/impl.hpp"

#include <wayland-server-core.h>

// std::erase(vector, ...) — impl.hpp already pulls in <vector>.

namespace ww {

namespace {
void device_release(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}
} // namespace

const struct wl_data_device_interface data_device_iface = {
    .start_drag = device_start_drag,
    .set_selection = device_set_selection,
    .release = device_release,
};

void device_resource_destroy(wl_resource *r) {
  auto *mgr = static_cast<DataDeviceManagerState *>(wl_resource_get_user_data(r));
  if (!mgr)
    return;
  std::erase(mgr->devices, r);

  // Dropping it from `devices` is not enough: an in-flight drag also holds
  // it as the current target, and every remaining drag event path
  // (drag_leave, drag_motion, drag_release, cancel_drag) posts an event to
  // that pointer without revalidating it. Leaving it set after the resource
  // is gone turns the next one into a use-after-free.
  //
  // This is reachable in ordinary operation -- a target client disconnecting
  // mid-drag -- and unavoidable at display teardown, where clients are reaped
  // in no particular order and a destroyed device can easily outlive nothing
  // at all while the drag source is torn down after it.
  if (mgr->drag_current_target_device == r) {
    mgr->drag_current_target_device = nullptr;
    mgr->drag_current_target_surface = nullptr;
    mgr->drag_current_offer = nullptr;
  }
}

} // namespace ww
