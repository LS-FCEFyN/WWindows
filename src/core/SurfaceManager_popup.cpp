#include "wwindows/core/surface_manager.hpp"
#include "wwindows/core/detail/surface_manager_impl.hpp"

#include <wwindows/support/log.hpp>

#include <algorithm>
#include <wwindows/adapters/wayland/protocols/xdg-shell-server-protocol.h>

namespace ww {

void SurfaceManager::create_popup(wl_resource *surface_res,
                                  wl_resource *parent_surface_res,
                                  wl_resource *popup_res, int32_t x,
                                  int32_t y, uint16_t width,
                                  uint16_t height) {
  auto sit = impl_->surfaces.find(surface_res);
  auto pit = impl_->surfaces.find(parent_surface_res);
  if (sit == impl_->surfaces.end() || pit == impl_->surfaces.end()) {
    WW_LOG_WARN("[popup] create_popup: unknown surface or parent");
    return;
  }

  if (!pit->second.x11_window) {
    // Per xdg-shell, "the parent of an xdg_popup must be mapped before the
    // xdg_popup itself" — a well-behaved client won't hit this.
    WW_LOG_WARN(
        "[popup] create_popup: parent surface has no X11 window yet");
    return;
  }

  if (sit->second.x11_window || sit->second.has_role()) {
    WW_LOG_WARN("[popup] create_popup: surface {} already has a role",
               static_cast<void *>(surface_res));
    return;
  }

  Surface &state = sit->second;
  Popup popup;
  popup.placement = ChildPlacement{parent_surface_res, x, y};
  popup.xdg_popup = popup_res;
  state.role = std::move(popup);
  pit->second.subsurface_children.push_back(surface_res);

  const uint32_t win = impl_->backend.create_child_window(
      pit->second.x11_window, static_cast<int16_t>(x),
      static_cast<int16_t>(y), width, height);
  impl_->backend.map_window(win);
  state.x11_window = win;
  state.x11_map_requested = true;
  impl_->surface_index.bind_window(win, surface_res);

  impl_->surface_index.push_popup(surface_res);

  WW_LOG_INFO("[popup] created (surface={}, parent={}, x11={}, "
             "pos=({},{}), size={}x{})",
             static_cast<void *>(surface_res),
             static_cast<void *>(parent_surface_res), win, x, y, width,
             height);
}

void SurfaceManager::reposition_popup(wl_resource *surface_res, int32_t x,
                                      int32_t y, uint16_t width,
                                      uint16_t height) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end() || !it->second.is_popup() ||
      !it->second.x11_window) {
    WW_LOG_TRACE("[popup] reposition ignored: surface {} not a mapped popup",
                static_cast<void *>(surface_res));
    return;
  }

  it->second.as_popup()->placement.x = x;
  it->second.as_popup()->placement.y = y;
  impl_->backend.move_window(it->second.x11_window, static_cast<int16_t>(x),
                             static_cast<int16_t>(y));
  impl_->backend.resize_window(it->second.x11_window, width, height);

  WW_LOG_TRACE("[popup] repositioned (surface={}, pos=({},{}), size={}x{})",
              static_cast<void *>(surface_res), x, y, width, height);
}

// Shared teardown for both client-initiated destroy (xdg_popup.destroy /
// wl_surface.destroy) and compositor-initiated dismissal
// (dismiss_popups_not_owning). Idempotent: safe to call on a surface that
// isn't (or is no longer) a popup.
void SurfaceManager::Impl::teardown_popup(Impl &impl, wl_resource *surface_res,
                                          Surface &state) {
  if (!state.is_popup())
    return;

  impl.surface_index.remove_popup(surface_res);

  if (wl_resource *parent = state.parent_surface()) {
    if (auto pit = impl.surfaces.find(parent);
        pit != impl.surfaces.end()) {
      auto &kids = pit->second.subsurface_children;
      kids.erase(std::remove(kids.begin(), kids.end(), surface_res),
                kids.end());
    }
  }

  if (state.x11_window) {
    impl.backend.destroy_window(state.x11_window);
    impl.surface_index.unbind_window(state.x11_window);
    state.x11_window = 0;
  }

  state.role = NoRole{};
}

void SurfaceManager::destroy_popup(wl_resource *surface_res) {
  auto sit = impl_->surfaces.find(surface_res);
  if (sit == impl_->surfaces.end())
    return; // wl_surface already gone; nothing left to unwind

  if (!sit->second.is_popup())
    return;

  WW_LOG_DEBUG("[popup] destroyed (surface={})",
              static_cast<void *>(surface_res));
  Impl::teardown_popup(*impl_, surface_res, sit->second);
}

void SurfaceManager::dismiss_popups_not_owning(wl_resource *clicked_surface) {
  if (impl_->surface_index.popups_empty())
    return;

  // Clicking on a mapped popup (or, transitively, one of its own child
  // popups — already covered since they're all in the same flat stack) is
  // interaction *with* the popup chain, not a dismissal.
  if (clicked_surface) {
    if (auto cit = impl_->surfaces.find(clicked_surface);
        cit != impl_->surfaces.end() && cit->second.is_popup())
      return;
  }

  WW_LOG_DEBUG("[popup] click outside popup chain — dismissing {} popup(s)",
              impl_->surface_index.popup_count());

  // Topmost (most recently opened) first — matches the order xdg-shell
  // requires clients to destroy nested popups in, and the order clients
  // expect popup_done events to arrive.
  while (!impl_->surface_index.popups_empty()) {
    wl_resource *surf = impl_->surface_index.top_popup();
    auto sit = impl_->surfaces.find(surf);
    if (sit == impl_->surfaces.end()) {
      impl_->surface_index.discard_top_popup();
      continue;
    }
    const auto *popup = sit->second.as_popup();
    wl_resource *popup_obj = popup ? popup->xdg_popup : nullptr;
    if (popup_obj)
      xdg_popup_send_popup_done(popup_obj);
    Impl::teardown_popup(*impl_, surf, sit->second); // also pops popup_stack
  }
}

} // namespace ww
