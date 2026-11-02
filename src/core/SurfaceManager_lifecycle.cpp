#include "wwindows/core/surface_manager.hpp"
#include "wwindows/core/detail/surface_manager_impl.hpp"

#include <wwindows/support/log.hpp>

#include <algorithm>

namespace ww {

SurfaceManager::SurfaceManager(IWindowBackend &backend, IResizeTimer *resize_timer)
    : impl_{std::make_unique<Impl>(backend, resize_timer)} {
  impl_->backend.set_resize_callback([this](uint32_t win, uint16_t w, uint16_t h) {
    request_resize(win, w, h);
  });
  // Generic (EWMH-level) counterpart to the resize wiring above -- see
  // set_wm_state()'s comment. A backend with nothing to report here (the
  // port's default no-op set_wm_state_callback) simply never calls back.
  impl_->backend.set_wm_state_callback(
      [this](uint32_t win, IWindowBackend::WmWindowState state) {
        set_wm_state(win, state);
      });
}

SurfaceManager::~SurfaceManager() {
  for (auto &[_, state] : impl_->surfaces)
    impl_->cancel_resize_settle(state);
  WW_LOG_INFO("[surface] manager shutting down ({} surface(s) still live)",
              impl_->surfaces.size());
}

void SurfaceManager::create_surface(wl_resource *resource) {
  impl_->surfaces.try_emplace(resource); // constructed in place: Surface is pinned
  WW_LOG_DEBUG("[surface] create surface {} (total: {})",
               static_cast<void *>(resource), impl_->surfaces.size());
}

void SurfaceManager::destroy_surface(wl_resource *resource) {
  auto it = impl_->surfaces.find(resource);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] destroy_surface: unknown resource {}",
                static_cast<void *>(resource));
    return;
  }

  impl_->cancel_resize_settle(it->second);
  *it->second.frame_callbacks_alive = false;
  it->second.presentation.lifetime->alive = false;
  ++it->second.presentation.lifetime->generation;
  for (const auto &weak_cb : it->second.all_frame_callbacks) {
    if (auto cb = weak_cb.lock(); cb && cb->resource)
      wl_resource_destroy(cb->resource);
  }
  it->second.frame_callbacks.clear();
  it->second.all_frame_callbacks.clear();
  it->second.presentation.cancel_import();

  if (it->second.pending.attached_buffer())
    wl_list_remove(&it->second.destroy_listener.link);
  if (it->second.last_buffer)
    wl_list_remove(&it->second.last_buffer_destroy_listener.link);
  if (const auto *t = it->second.as_toplevel()) {
    if (t->toplevel)
      wl_list_remove(&it->second.toplevel_destroy_listener.link);
    if (t->xdg_surface)
      wl_list_remove(&it->second.xdg_surface_destroy_listener.link);
  }

  it->second.pending.attach.reset();
  it->second.last_buffer = nullptr;

  // A cursor surface owns a window-system cursor object rather than a window.
  if (auto *cursor = it->second.as_cursor(); cursor && cursor->handle) {
    impl_->backend.free_cursor(cursor->handle);
    cursor->handle = 0;
    cursor->changed = nullptr;
  }

  // PendingRestack::sibling is a raw wl_resource*; drop every reference to
  // this surface before it dies so a recycled address can never be resolved
  // against an unrelated surface by a later parent apply. Only this surface's
  // siblings (its parent's children) and its own children can hold one.
  {
    auto clear_refs = [&](const std::vector<wl_resource *> &holders) {
      for (wl_resource *holder : holders) {
        if (auto hit = impl_->surfaces.find(holder);
            hit != impl_->surfaces.end())
          if (auto *sub = hit->second.as_subsurface();
              sub && sub->pending_restack &&
              sub->pending_restack->sibling == resource)
            sub->pending_restack->sibling = nullptr;
      }
    };
    if (wl_resource *parent = it->second.parent_surface())
      if (auto pit = impl_->surfaces.find(parent);
          pit != impl_->surfaces.end())
        clear_refs(pit->second.subsurface_children);
    clear_refs(it->second.subsurface_children);
  }

  // If this surface is itself a subsurface or popup, unlink it from its
  // parent's child list so the parent doesn't keep a dangling reference.
  if (wl_resource *parent = it->second.parent_surface()) {
    if (auto pit = impl_->surfaces.find(parent);
        pit != impl_->surfaces.end()) {
      auto &kids = pit->second.subsurface_children;
      kids.erase(std::remove(kids.begin(), kids.end(), resource), kids.end());
    }
  }

  // A wl_surface can be destroyed directly (client tears down its whole
  // surface tree) without xdg_popup.destroy ever running first — make sure
  // it doesn't linger in the dismiss stack.
  if (it->second.is_popup()) {
    impl_->surface_index.remove_popup(resource);
  }

  // If this surface is a parent to subsurfaces, X11 will destroy their
  // child windows automatically as soon as this window is destroyed below —
  // clear our bookkeeping for them now so we never call destroy_window() a
  // second time on an id X has already recycled.
  //
  // X11 destroys the whole descendant tree, not just direct children, so the
  // same applies to grandchildren and deeper: forget every descendant's window
  // (otherwise a later destroy of a grandchild hits an already-dead id, and
  // surface_for_window() keeps routing events for it).
  auto forget_descendants = [this](auto &&self, wl_resource *parent_res) -> void {
    const auto pit = impl_->surfaces.find(parent_res);
    if (pit == impl_->surfaces.end())
      return;
    for (wl_resource *child_res : pit->second.subsurface_children) {
      const auto cit = impl_->surfaces.find(child_res);
      if (cit == impl_->surfaces.end())
        continue;
      self(self, child_res);
      if (cit->second.x11_window) {
        impl_->backend.forget_window(cit->second.x11_window);
        impl_->surface_index.unbind_window(cit->second.x11_window);
        cit->second.x11_window = 0;
      }
    }
  };
  forget_descendants(forget_descendants, resource);
  for (wl_resource *child_res : it->second.subsurface_children) {
    if (auto cit = impl_->surfaces.find(child_res); cit != impl_->surfaces.end())
      if (auto *placement = cit->second.placement())
        placement->parent = nullptr;
  }

  if (it->second.x11_window) {
    impl_->backend.destroy_window(it->second.x11_window);
    WW_LOG_DEBUG("[surface] destroyed {} (x11 win {})",
                 static_cast<void *>(resource), it->second.x11_window);
    impl_->surface_index.unbind_window(it->second.x11_window);
  } else {
    WW_LOG_DEBUG("[surface] destroyed {} (never became a toplevel)",
                 static_cast<void *>(resource));
  }
  impl_->surfaces.erase(it);
}

wl_resource *SurfaceManager::surface_for_window(uint32_t x11_window) const {
  auto it = impl_->surface_index.find_window(x11_window);
  return (it != impl_->surface_index.window_end()) ? it->second : nullptr;
}

void SurfaceManager::surface_local_origin(wl_resource *surface, int32_t &x,
                                          int32_t &y) const {
  x = 0;
  y = 0;
  auto it = impl_->surfaces.find(surface);
  if (it == impl_->surfaces.end() || !it->second.committed.geometry)
    return;
  x = it->second.committed.geometry->x;
  y = it->second.committed.geometry->y;
}

} // namespace ww
