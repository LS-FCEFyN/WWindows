#include "wwindows/core/surface_manager.hpp"
#include "wwindows/core/detail/surface_manager_impl.hpp"

#include <wwindows/support/log.hpp>

#include <algorithm>
#include <wayland-server-core.h>

namespace ww {

void SurfaceManager::Impl::subsurface_origin(const Surface &parent,
                                             int32_t x, int32_t y,
                                             int16_t &out_x,
                                             int16_t &out_y) const {
  const int32_t dx = parent.committed.geometry ? parent.committed.geometry->x : 0;
  const int32_t dy = parent.committed.geometry ? parent.committed.geometry->y : 0;
  out_x = static_cast<int16_t>(x - dx);
  out_y = static_cast<int16_t>(y - dy);
}

void SurfaceManager::Impl::reposition_children(const Surface &parent) {
  for (wl_resource *child : parent.subsurface_children) {
    auto cit = surfaces.find(child);
    if (cit == surfaces.end() || !cit->second.x11_window)
      continue;
    const auto *sub = cit->second.as_subsurface();
    if (!sub)
      continue;
    int16_t cx = 0;
    int16_t cy = 0;
    subsurface_origin(parent, sub->placement.x, sub->placement.y, cx, cy);
    backend.move_window(cit->second.x11_window, cx, cy);
  }
}

bool SurfaceManager::Impl::is_effectively_synchronized(
    const Surface &state, int depth) const {
  const auto *sub = state.as_subsurface();
  if (!sub)
    return false;
  if (sub->sync)
    return true;
  if (depth > 64) // a parent cycle nothing currently rejects at creation
    return true;  // -- err toward caching rather than an infinite recursion
  auto pit = surfaces.find(sub->placement.parent);
  if (pit == surfaces.end())
    return false;
  return is_effectively_synchronized(pit->second, depth + 1);
}

void SurfaceManager::Impl::cascade_to_children(Surface &parent_state,
                                               int depth) {
  if (depth > 64) // same cycle guard as is_effectively_synchronized()
    return;

  for (wl_resource *child : parent_state.subsurface_children) {
    auto cit = surfaces.find(child);
    if (cit == surfaces.end())
      continue;
    Surface &cs = cit->second;

    // Only a wl_subsurface has parent-timed position, restack and cached
    // state. A popup child (also listed in subsurface_children) is positioned
    // by its xdg_positioner and must not be touched here.
    if (auto *sub = cs.as_subsurface()) {
      // Position: always timed to the parent's apply, regardless of this
      // child's own sync mode -- see Subsurface::pending_position. Applied
      // only when a position was queued and differs, so an unrelated parent
      // apply does not spam move_window calls.
      if (sub->pending_position) {
        const SubsurfacePosition target = *sub->pending_position;
        sub->pending_position.reset();
        const bool moved =
            target != SubsurfacePosition{sub->placement.x, sub->placement.y};
        sub->placement.x = target.x;
        sub->placement.y = target.y;
        if (cs.x11_window && moved) {
          int16_t cx = 0;
          int16_t cy = 0;
          subsurface_origin(parent_state, target.x, target.y, cx, cy);
          backend.move_window(cs.x11_window, cx, cy);
        }
      }

      // Restack: same deferred-to-parent-apply timing -- see PendingRestack.
      if (sub->pending_restack) {
        const PendingRestack restack = *sub->pending_restack;
        sub->pending_restack.reset();
        uint32_t sibling_win = 0;
        if (restack.sibling && restack.sibling != child) {
          auto sib_it = surfaces.find(restack.sibling);
          if (sib_it != surfaces.end())
            sibling_win = sib_it->second.x11_window; // may still be 0; fine
        }
        if (cs.x11_window)
          backend.restack_window(cs.x11_window, sibling_win, restack.above);
      }

      // This child's own wl_surface state, if a synchronized commit left any
      // cached.
      if (sub->has_cached_state) {
        sub->has_cached_state = false;
        apply_surface_state(child, cs);
      }
    }

    // A grandchild may have state waiting on *this* child's apply, which
    // just happened above (position/restack unconditionally; buffer
    // state if it was cached) -- recurse regardless of whether this
    // child itself had anything pending.
    cascade_to_children(cs, depth + 1);
  }
}

// Realize (or re-check) the X11 child window for a subsurface. Called from
// create_subsurface() for the common case where the parent is already a
// mapped toplevel, and again from SurfaceManager::commit() to cover a
// subsurface created before its parent got its own X11 window.
void SurfaceManager::Impl::ensure_subsurface_window(wl_resource *surface_res,
                                                     Surface &state) {
  auto *sub = state.as_subsurface();
  if (state.x11_window || !sub || !sub->placement.parent)
    return;

  auto pit = surfaces.find(sub->placement.parent);
  if (pit == surfaces.end() || !pit->second.x11_window) {
    WW_LOG_TRACE("[subsurface] parent not realized yet (surface={})",
                 static_cast<void *>(surface_res));
    return;
  }

  // 1x1 placeholder: the real size is only known once the client attaches
  // its first buffer, at which point commit() resizes to match.
  int16_t cx = 0;
  int16_t cy = 0;
  subsurface_origin(pit->second, sub->placement.x, sub->placement.y, cx, cy);
  const uint32_t win =
      backend.create_child_window(pit->second.x11_window, cx, cy, 1, 1);
  backend.map_window(win);
  state.x11_window = win;
  state.x11_map_requested = true;
  surface_index.bind_window(win, surface_res);

  WW_LOG_INFO(
      "[subsurface] realized (surface={}, parent_win={}, x11={}, pos=({},{}))",
      static_cast<void *>(surface_res), pit->second.x11_window, win,
      sub->placement.x, sub->placement.y);
}

void SurfaceManager::create_subsurface(wl_resource *subsurface_res,
                                       wl_resource *surface_res,
                                       wl_resource *parent_res) {
  (void)subsurface_res;

  auto sit = impl_->surfaces.find(surface_res);
  auto pit = impl_->surfaces.find(parent_res);
  if (sit == impl_->surfaces.end() || pit == impl_->surfaces.end()) {
    WW_LOG_WARN("[subsurface] get_subsurface: unknown surface or parent");
    return;
  }

  if (sit->second.x11_window || sit->second.has_role()) {
    WW_LOG_WARN(
        "[subsurface] get_subsurface: surface {} already has a role",
        static_cast<void *>(surface_res));
    return;
  }

  Subsurface sub;
  sub.placement.parent = parent_res;
  sit->second.role = std::move(sub);
  pit->second.subsurface_children.push_back(surface_res);

  WW_LOG_DEBUG("[subsurface] created (surface={}, parent={})",
              static_cast<void *>(surface_res),
              static_cast<void *>(parent_res));

  impl_->ensure_subsurface_window(surface_res, sit->second);
}

void SurfaceManager::destroy_subsurface_role(wl_resource *surface_res) {
  auto sit = impl_->surfaces.find(surface_res);
  if (sit == impl_->surfaces.end())
    return; // wl_surface was already destroyed first; nothing to unwind

  Surface &state = sit->second;
  if (!state.is_subsurface())
    return;

  if (wl_resource *parent = state.parent_surface()) {
    if (auto pit = impl_->surfaces.find(parent);
        pit != impl_->surfaces.end()) {
      auto &kids = pit->second.subsurface_children;
      kids.erase(std::remove(kids.begin(), kids.end(), surface_res),
                kids.end());
    }
  }

  if (state.x11_window) {
    impl_->backend.destroy_window(state.x11_window);
    impl_->surface_index.unbind_window(state.x11_window);
    state.x11_window = 0;
  }

  WW_LOG_DEBUG("[subsurface] role removed (surface={})",
              static_cast<void *>(surface_res));

  // Back to no role; a later get_subsurface starts from the spec's initial
  // values (synchronized, nothing pending, nothing cached).
  state.role = NoRole{};
}

void SurfaceManager::set_subsurface_position(wl_resource *surface_res,
                                             int32_t x, int32_t y) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end() || !it->second.is_subsurface()) {
    WW_LOG_WARN("[subsurface] set_position on non-subsurface {}",
                static_cast<void *>(surface_res));
    return;
  }

  // Double-buffered against the *parent's* state being applied, not this
  // call -- see Subsurface::pending_position.
  // "a new request always replaces the scheduled position from any
  // previous request" is exactly a plain overwrite here, no accumulation.
  it->second.as_subsurface()->pending_position = SubsurfacePosition{x, y};

  WW_LOG_TRACE(
      "[subsurface] set_position scheduled, waiting on parent's apply "
      "(surface={}, pos=({},{}))",
      static_cast<void *>(surface_res), x, y);
}

void SurfaceManager::place_subsurface(wl_resource *surface_res,
                                      wl_resource *sibling_res, bool above) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end() || !it->second.is_subsurface()) {
    WW_LOG_WARN("[subsurface] place_{} on non-subsurface {}",
                above ? "above" : "below",
                static_cast<void *>(surface_res));
    return;
  }

  // Double-buffered against the parent's apply, same as set_position --
  // see PendingRestack for the single-pending-op simplification against the
  // spec's full pending order. Not gated on x11_window existing yet
  // (cascade_to_children() checks that when it actually restacks).
  it->second.as_subsurface()->pending_restack =
      PendingRestack{sibling_res, above};

  WW_LOG_TRACE(
      "[subsurface] place_{} scheduled, waiting on parent's apply "
      "(surface={}, sibling={})",
      above ? "above" : "below", static_cast<void *>(surface_res),
      static_cast<void *>(sibling_res));
}

void SurfaceManager::set_subsurface_sync(wl_resource *surface_res,
                                         bool sync) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end() || !it->second.is_subsurface()) {
    WW_LOG_WARN("[subsurface] set_{} on non-subsurface {}",
                sync ? "sync" : "desync", static_cast<void *>(surface_res));
    return;
  }
  Surface &state = it->second;

  // "set_sync and set_desync are effective immediately" -- the mode
  // change itself, as opposed to the cached wl_surface state set_desync
  // may also need to flush below, which is a separate, conditional
  // effect.
  state.as_subsurface()->sync = sync;

  WW_LOG_TRACE("[subsurface] set_{} (surface={})", sync ? "sync" : "desync",
              static_cast<void *>(surface_res));

  if (sync || !state.as_subsurface()->has_cached_state)
    return;

  // "If a surface's parent surface behaves as desynchronized, then the
  // cached state is applied on set_desync." Our own new mode is already
  // desync (just set above); what matters now is whether an ancestor
  // still forces us synchronized despite that -- is_effectively_
  // synchronized() is exactly that check, and happens to also cover the
  // "no parent found" edge case the same way the rest of this file does.
  if (impl_->is_effectively_synchronized(state))
    return;

  state.as_subsurface()->has_cached_state = false;
  impl_->apply_surface_state(surface_res, state);
  impl_->cascade_to_children(state);
}

} // namespace ww
