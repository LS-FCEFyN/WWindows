// wl_surface.set_input_region tracking (see IWindowBackend::set_input_shape)
// and wl_pointer.set_cursor role registration — two small, independent
// bits of Surface bookkeeping that don't warrant their own files.
#include "wwindows/core/surface_manager.hpp"
#include "wwindows/core/detail/surface_manager_impl.hpp"

#include <wwindows/support/log.hpp>

#include <utility>

namespace ww {

void SurfaceManager::set_pending_input_region(wl_resource *surface_res,
                                              std::vector<InputRect> add_rects,
                                              std::vector<InputRect> sub_rects,
                                              bool has_region) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] set_input_region: unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }

  Surface &state = it->second;
  state.pending.input_region = PendingInputRegion{
      has_region, std::move(add_rects), std::move(sub_rects)};

  WW_LOG_TRACE(
      "[surface] set_input_region (surface={}, has_region={}, add={}, sub={})",
      static_cast<void *>(surface_res), has_region,
      state.pending.input_region->add.size(),
      state.pending.input_region->sub.size());
}

void SurfaceManager::set_cursor_surface(wl_resource *surface_res,
                                        int32_t hotspot_x, int32_t hotspot_y,
                                        CursorChangedCallback on_changed) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] set_cursor_surface: unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }

  Surface &state = it->second;
  // A surface holds one role. wl_pointer.set_cursor on a surface that is
  // already a toplevel, subsurface or popup is a client protocol error; stay
  // defensive and leave that surface's window alone.
  if (state.x11_window || (state.has_role() && !state.is_cursor())) {
    WW_LOG_WARN("[surface] set_cursor_surface: surface {} already has a "
                "different role",
                static_cast<void *>(surface_res));
    return;
  }

  // Calling set_cursor again on a surface that already is the cursor keeps
  // its handle (and the X cursor object it owns); only a new role starts one.
  if (!state.is_cursor())
    state.role = Cursor{};
  Cursor &cursor = *state.as_cursor();
  const bool hotspot_changed =
      cursor.hotspot_x != hotspot_x || cursor.hotspot_y != hotspot_y;
  cursor.hotspot_x = hotspot_x;
  cursor.hotspot_y = hotspot_y;
  cursor.changed = std::move(on_changed);

  // Content that arrived before the role did:
  //  * a commit that found no role and was parked (commit_parked) is
  //    applied now, exactly as if it had come after set_cursor;
  //  * a surface that was already a cursor, or committed content earlier,
  //    but whose hotspot just changed needs its X cursor rebuilt, because the
  //    hotspot is baked into the cursor object.
  if (state.commit_parked) {
    state.commit_parked = false;
    impl_->apply_cursor_surface_state(surface_res, state);
  } else if (state.last_buffer && (hotspot_changed || !cursor.handle)) {
    impl_->rebuild_cursor(state, state.last_buffer);
  }
}

void SurfaceManager::clear_cursor_surface(wl_resource *surface_res) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end())
    return;

  Surface &state = it->second;
  if (!state.is_cursor())
    return;

  if (state.as_cursor()->handle)
    impl_->backend.free_cursor(state.as_cursor()->handle);
  state.commit_parked = false;
  state.role = NoRole{}; // drops the handle and callback with the role
}

uint32_t SurfaceManager::cursor_for_surface(wl_resource *surface_res) const {
  const auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end() || !it->second.is_cursor())
    return 0;
  return it->second.as_cursor()->handle;
}

} // namespace ww
