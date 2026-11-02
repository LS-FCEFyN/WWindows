#include "wwindows/core/surface_manager.hpp"
#include "wwindows/core/detail/surface_manager_impl.hpp"
#include <wwindows/core/policy/surface_geometry.hpp>
#include <wwindows/core/detail/pixel_scale.hpp>
#include <wwindows/core/detail/frame_callbacks.hpp>
#include <chrono>
#include <wwindows/support/frame_diag.hpp>
#include <wwindows/ports/imported_buffer.hpp>

#include <wwindows/support/log.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <time.h>
#include <vector>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

namespace ww {

namespace {

void append_completion(std::function<void()> &current,
                       std::function<void()> next) {
  if (!next)
    return;
  if (!current) {
    current = std::move(next);
    return;
  }
  auto previous = std::move(current);
  current = [previous = std::move(previous), next = std::move(next)]() mutable {
    previous();
    next();
  };
}

IWindowBackend::FramePresentedCallback track_dmabuf_source_read(
    ImportedBuffer &buffer_state, wl_resource *buffer_resource) {
  const auto use_state = buffer_state.use_state;
  const std::weak_ptr<bool> alive = buffer_state.alive;
  ++use_state->source_reads_in_flight;
  return [use_state, alive, buffer_resource]() {
    if (use_state->source_reads_in_flight > 0)
      --use_state->source_reads_in_flight;
    if (use_state->source_reads_in_flight != 0 ||
        !use_state->release_pending)
      return;
    use_state->release_pending = false;
    const auto resource_alive = alive.lock();
    if (resource_alive && *resource_alive)
    {
      wl_buffer_send_release(buffer_resource);
      ::ww::diag::note(0, ::ww::diag::Ev::Release);
    }
  };
}

// The WM target is separate from configure history and actual X11 geometry.
// Stale retained content is stretched to this target while the resize settles.
uint16_t resize_target_width(const Surface &state) {
  return state.resize_target().width;
}

uint16_t resize_target_height(const Surface &state) {
  return state.resize_target().height;
}

constexpr uint64_t kMaxStretchPixels = 8192ull * 8192ull;

bool can_stretch_to_target(const Surface &state) {
  return static_cast<uint64_t>(resize_target_width(state)) *
             resize_target_height(state) <=
         kMaxStretchPixels;
}

void present_stretched_to_target(IWindowBackend &backend,
                                 const Surface &state,
                                 const void *source, uint32_t source_stride,
                                 uint32_t source_width,
                                 uint32_t source_height, BufferRegion region,
                                 IWindowBackend::FramePresentedCallback done) {
  const auto scaled = detail::scale_shm_32_to_size(
      source, source_stride, source_width, source_height, region,
      resize_target_width(state), resize_target_height(state));
  if (scaled.bytes.empty()) {
    if (done)
      done();
    return;
  }
  backend.present_pixels(state.x11_window, scaled.width, scaled.height,
                         scaled.stride, scaled.bytes.data(), BufferRegion{},
                         std::move(done));
}

} // namespace

void SurfaceManager::mark_surface_damage(wl_resource *surface,
                                         int32_t width, int32_t height) {
  // Empty rectangles do not damage any pixels. Coordinates are intentionally
  // not retained: until the backend supports partial updates, a non-empty
  // rectangle conservatively requests a full-buffer repaint on commit.
  if (width <= 0 || height <= 0)
    return;
  auto it = impl_->surfaces.find(surface);
  if (it == impl_->surfaces.end()) {
    WW_LOG_TRACE("[surface] damage on unknown surface {}",
                 static_cast<void *>(surface));
    return;
  }
  it->second.pending.damaged = true;
}

void SurfaceManager::attach_buffer(wl_resource *surface, wl_resource *buffer) {
  auto it = impl_->surfaces.find(surface);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] attach: unknown surface {}",
                static_cast<void *>(surface));
    return;
  }

  if (wl_resource *previous = it->second.pending.attached_buffer()) {
    wl_list_remove(&it->second.destroy_listener.link);
    wl_list_init(&it->second.destroy_listener.link);
    if (previous != buffer) {
      wl_buffer_send_release(previous);
      ::ww::diag::note(it->second.x11_window, ::ww::diag::Ev::Release, 1);
    }
  }

  if (buffer)
    it->second.pending.attach = PendingAttachBuffer{buffer};
  else
    it->second.pending.attach = PendingDetach{};

  if (buffer) {
    it->second.destroy_listener.notify = Impl::on_resource_destroyed;
    wl_resource_add_destroy_listener(buffer, &it->second.destroy_listener);
  }

  WW_LOG_TRACE("[surface] attach (surface={}, buffer={})",
               static_cast<void *>(surface), static_cast<void *>(buffer));
}

void SurfaceManager::add_frame_callback(wl_resource *surface, wl_resource *callback) {
  auto it = impl_->surfaces.find(surface);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] frame callback on unknown surface {}",
                static_cast<void *>(surface));
    return;
  }
  auto frame_callback = std::make_shared<FrameCallbackState>();
  frame_callback->resource = callback;
  frame_callback->surface_alive = it->second.frame_callbacks_alive;
  frame_callback->destroy_listener.notify = FrameCallbackState::on_destroy;
  wl_resource_add_destroy_listener(callback, &frame_callback->destroy_listener);
  auto &all = it->second.all_frame_callbacks;
  all.erase(std::remove_if(all.begin(), all.end(),
                           [](const auto &weak) { return weak.expired(); }),
            all.end());
  all.emplace_back(frame_callback);
  it->second.frame_callbacks.push_back(std::move(frame_callback));
  ::ww::diag::note(it->second.x11_window, ::ww::diag::Ev::FcbQueued);
  WW_LOG_TRACE("[surface] frame callback queued (surface={}, total={})",
               static_cast<void *>(surface), it->second.frame_callbacks.size());
}

void SurfaceManager::set_pending_offset(wl_resource *surface_res, int32_t x,
                                        int32_t y) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] offset on unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }
  it->second.pending.offset_x = x;
  it->second.pending.offset_y = y;
  WW_LOG_TRACE("[surface] pending offset ({}, {}) (surface={})", x, y,
               static_cast<void *>(surface_res));
}

void SurfaceManager::set_pending_buffer_scale(wl_resource *surface_res,
                                              int32_t scale) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] buffer_scale on unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }
  it->second.pending.buffer_scale = scale;
}

void SurfaceManager::set_pending_viewport_source(wl_resource *surface_res,
                                                 wl_fixed_t x, wl_fixed_t y,
                                                 wl_fixed_t width, wl_fixed_t height) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[viewporter] set_source on unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }
  const wl_fixed_t minus_one = wl_fixed_from_int(-1);
  if (x == minus_one && y == minus_one && width == minus_one && height == minus_one) {
    it->second.pending.viewport_src_unset = true;
    WW_LOG_TRACE("[viewporter] pending source unset (surface={})",
                 static_cast<void *>(surface_res));
    return;
  }
  it->second.pending.viewport_src_x = x;
  it->second.pending.viewport_src_y = y;
  it->second.pending.viewport_src_width = width;
  it->second.pending.viewport_src_height = height;
  it->second.pending.viewport_src_unset = false;
  WW_LOG_TRACE("[viewporter] pending source x={} y={} w={} h={} (surface={})",
               wl_fixed_to_double(x), wl_fixed_to_double(y),
               wl_fixed_to_double(width), wl_fixed_to_double(height),
               static_cast<void *>(surface_res));
}

void SurfaceManager::set_pending_viewport_destination(wl_resource *surface_res,
                                                      int32_t width, int32_t height) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[viewporter] set_destination on unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }
  if (width == -1 && height == -1) {
    it->second.pending.viewport_dst_unset = true;
    WW_LOG_TRACE("[viewporter] pending destination unset (surface={})",
                 static_cast<void *>(surface_res));
    return;
  }
  it->second.pending.viewport_dst_width = width;
  it->second.pending.viewport_dst_height = height;
  it->second.pending.viewport_dst_unset = false;
  WW_LOG_TRACE("[viewporter] pending destination {}x{} (surface={})",
               width, height, static_cast<void *>(surface_res));
}

void SurfaceManager::commit(wl_resource *surface) {
  auto it = impl_->surfaces.find(surface);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] commit: unknown surface {}",
                static_cast<void *>(surface));
    return;
  }
  Surface &state = it->second;

  // wl_subsurface: in synchronized mode (this subsurface's own mode, or
  // any ancestor's), a commit only caches state -- see
  // is_effectively_synchronized()'s own comment. The cache is just
  // whatever is already sitting in the pending_* fields apply_surface_
  // state() below would otherwise consume; nothing to actually move
  // into it here. Checked before ensure_subsurface_window/x11_window
  // even come into it -- those live inside apply_surface_state() itself,
  // unchanged, so a synchronized child that was never realized gets one
  // more chance to realize at the point its cached state is actually
  // applied (by which time its parent, having just applied its own
  // state, definitely has a window), rather than only on its own next
  // direct commit.
  if (impl_->is_effectively_synchronized(state)) {
    state.as_subsurface()->has_cached_state = true;
    WW_LOG_TRACE(
        "[subsurface] commit cached, waiting on parent's apply (surface={})",
        static_cast<void *>(surface));
    return;
  }

  impl_->apply_surface_state(surface, state);
  impl_->cascade_to_children(state);
}

void SurfaceManager::Impl::apply_surface_state(wl_resource *surface,
                                               Surface &state) {
  // A wl_pointer.set_cursor surface has no X11 window: its buffer becomes a
  // window-system cursor, not window contents.
  if (state.is_cursor()) {
    apply_cursor_surface_state(surface, state);
    return;
  }

  // A subsurface created before its parent had an X11 window of its own
  // gets realized here, lazily, once that's no longer true.
  if (state.is_subsurface() && !state.x11_window)
    ensure_subsurface_window(surface, state);

  if (!state.x11_window) {
    // Common and expected: a client can commit a wl_surface before it has
    // been given a role yet (xdg_toplevel, subsurface, ...) — no window to
    // present into yet. Remembered, because the role about to arrive might be
    // the cursor one, which has no window to wait for (see
    // Surface::commit_parked).
    state.commit_parked = true;
    WW_LOG_TRACE("[surface] commit before role assigned (surface={})",
                 static_cast<void *>(surface));
    // Nothing will be presented for this commit, but the client is still
    // waiting on its wl_surface.frame callbacks. Firefox paces its whole
    // refresh loop on them and will not commit again until one fires, so a
    // callback left parked here freezes the window for good. Complete them
    // after one frame interval (not immediately, so a role-less surface
    // cannot make the client spin).
    if (!state.frame_callbacks.empty()) {
      WW_LOG_WARN("[diag] commit parked without an X11 window (surface={}, "
                  "subsurface={}, has_parent={}, callbacks={}) -- completing "
                  "its frame callbacks",
                  static_cast<void *>(surface), state.is_subsurface(),
                  state.parent_surface() != nullptr,
                  state.frame_callbacks.size());
      auto callbacks = std::move(state.frame_callbacks);
      state.frame_callbacks.clear();
      auto parked_done = detail::make_frame_completion(
          std::move(callbacks), state.frame_callbacks_alive, 0);
      if (parked_done) {
        if (resize_timer) {
          resize_timer->schedule_at(
              resize_timer->now() + std::chrono::milliseconds{16},
              [done = std::move(parked_done)]() mutable { done(); });
        } else {
          parked_done();
        }
      }
    }
    return;
  }

  wl_resource *const pending_buffer = state.pending.attached_buffer();
  const bool attachment_requested = state.pending.attach.has_value();
  const bool attaching_buffer = pending_buffer != nullptr;
  const bool detaching_buffer = state.pending.detach_requested();
  const bool geometry_updated = state.pending.window_geometry.has_value();
  const bool offset_updated = state.pending.offset_x != 0 ||
                              state.pending.offset_y != 0;
  const bool damaged = state.pending.damaged;
  state.pending.damaged = false;

  // Any applied commit supersedes an earlier asynchronous presentation
  // attempt unless it is another commit of the same dmabuf currently being
  // imported. That case updates the pending presentation request below.
  ++state.presentation.lifetime->generation;

  ::ww::diag::note(state.x11_window, ::ww::diag::Ev::Commit,
                   static_cast<uint32_t>(state.frame_callbacks.size()));
  auto callbacks = std::move(state.frame_callbacks);
  state.frame_callbacks.clear();
  auto frame_done = detail::make_frame_completion(
      std::move(callbacks), state.frame_callbacks_alive, state.x11_window);
  bool frame_callback_submitted = false;

  // wl_surface.offset (and wl_surface.attach's deprecated x/y, which feeds
  // the same pending fields) is double-buffered like everything else here:
  // apply and reset now, before anything below reads offset_{x,y}. Always
  // run, not just when non-zero, so a commit that declares no offset
  // correctly leaves the running total exactly where it was.
  state.committed.offset_x += state.pending.offset_x;
  state.committed.offset_y += state.pending.offset_y;
  state.pending.offset_x = 0;
  state.pending.offset_y = 0;

  // Window geometry: double-buffered against this same commit -- only
  // touched when set_window_geometry was actually called since the last
  // commit; otherwise whatever geometry is already active carries over.
  if (state.pending.window_geometry) {
    const std::optional<WindowGeometry> before = state.committed.geometry;
    state.committed.geometry = *state.pending.window_geometry;
    state.pending.window_geometry.reset();

    // Children are positioned relative to this surface's origin, but their
    // X11 windows are children of a window whose origin is now the
    // *geometry* origin (see surface_local_origin()) -- moving it moves all
    // of them. Comparing effective origins (0,0 when no geometry is
    // declared, same rule surface_local_origin() uses) covers geometry
    // being cleared or first declared, not just an existing one changing.
    const int32_t new_origin_x = state.committed.geometry ? state.committed.geometry->x : 0;
    const int32_t new_origin_y = state.committed.geometry ? state.committed.geometry->y : 0;
    const int32_t old_origin_x = before ? before->x : 0;
    const int32_t old_origin_y = before ? before->y : 0;
    if (new_origin_x != old_origin_x || new_origin_y != old_origin_y)
      reposition_children(state);
  }

  const int32_t previous_buffer_scale = state.committed.buffer_scale;
  if (state.pending.buffer_scale) {
    state.committed.buffer_scale = std::max<int32_t>(*state.pending.buffer_scale, 1);
    state.pending.buffer_scale.reset();
  }
  const bool buffer_scale_changed =
      previous_buffer_scale != state.committed.buffer_scale;

  // wp_viewport: apply pending viewport state (double-buffered)
  if (!state.pending.viewport_src_unset) {
    state.committed.viewport_src_x = state.pending.viewport_src_x;
    state.committed.viewport_src_y = state.pending.viewport_src_y;
    state.committed.viewport_src_width = state.pending.viewport_src_width;
    state.committed.viewport_src_height = state.pending.viewport_src_height;
    state.committed.viewport_src_unset = false;
  } else {
    state.committed.viewport_src_unset = true;
  }
  if (!state.pending.viewport_dst_unset) {
    state.committed.viewport_dst_width = state.pending.viewport_dst_width;
    state.committed.viewport_dst_height = state.pending.viewport_dst_height;
    state.committed.viewport_dst_unset = false;
  } else {
    state.committed.viewport_dst_unset = true;
  }

  // The commit that ends a toplevel's setup. Everything the client said
  // about the window since get_toplevel (title, app_id, parent, ...) is
  // already on the X11 window; the size limits are double-buffered and
  // arrive with this commit. Only then is the window mapped, so the window
  // manager's first look at it -- which is when it decides workspace,
  // floating and size -- sees a fully described window.
  if (const auto *top = state.as_toplevel(); top && top->toplevel) {
    if (state.pending.size_limits) {
      state.committed.size_limits = *state.pending.size_limits;
      state.pending.size_limits.reset();
      backend.set_window_size_limits(state.x11_window, state.committed.size_limits);
    }
    if (!state.x11_map_requested && !detaching_buffer) {
      backend.map_window(state.x11_window);
      state.x11_map_requested = true;
      WW_LOG_INFO("[surface] toplevel mapped on first commit (surface={}, "
                  "x11={})",
                  static_cast<void *>(surface), state.x11_window);
    }
  }

  if (state.pending.input_region) {
    const PendingInputRegion &region = *state.pending.input_region;
    // wl_surface.set_input_region's rects are surface-local — the same
    // coordinate space xdg_surface.set_window_geometry's rect is in — but
    // the X11 window they're being applied to now sits at the *geometry*
    // rect's origin, not the buffer's, per the window-geometry crop
    // above; geometry.x/y - offset_x/y is exactly that crop's
    // origin in this buffer's own pixel space (see
    // Surface::offset_{x,y}'s doc comment for the offset half of
    // that). Applied raw, a region a client declared to line up with its
    // own window geometry (the common case: CSD toolkits set the input
    // region to exclude their own drop shadow, matching the geometry
    // rect almost exactly) ends up offset from the window by however big
    // that shadow margin is, potentially leaving most or all of the
    // actual window outside its own input-sensitive area.
    const int32_t dx = state.committed.geometry ? state.committed.geometry->x - state.committed.offset_x : 0;
    const int32_t dy = state.committed.geometry ? state.committed.geometry->y - state.committed.offset_y : 0;
    if (dx == 0 && dy == 0) {
      backend.set_input_shape(state.x11_window, region.has_region,
                                     region.add, region.sub);
    } else {
      const auto translate = [dx, dy](const std::vector<InputRect> &rects) {
        std::vector<InputRect> out;
        out.reserve(rects.size());
        for (const auto &r : rects) {
          out.push_back({static_cast<int16_t>(r.x - dx),
                         static_cast<int16_t>(r.y - dy), r.width, r.height});
        }
        return out;
      };
      backend.set_input_shape(state.x11_window, region.has_region,
                                     translate(region.add),
                                     translate(region.sub));
    }
    state.pending.input_region.reset();
  }

  wl_resource *buffer_to_present =
      attaching_buffer ? pending_buffer : nullptr;

  if (state.presentation.importing) {
    auto pending = state.presentation.importing;
    const bool still_same_buffer =
        !detaching_buffer &&
        (buffer_to_present == pending->buffer ||
         (!buffer_to_present && state.last_buffer == pending->buffer));
    if (!still_same_buffer) {
      // A different buffer or explicit detach superseded this import. Its
      // callback may cache the completed import on the wl_buffer, but must
      // not present it over the newer committed state. Cancel the old frame
      // callbacks immediately rather than holding them until import completes.
      //
      // "Cancel" must not mean "never deliver": dropping the std::function
      // destroyed the wl_callback resources without sending wl_callback.done,
      // so a client that paces on them (Firefox) waited forever and its
      // window stopped updating. The commit is superseded, not its callbacks:
      // they complete together with the newer frame.
      auto superseded = std::move(pending->on_presented);
      pending->on_presented = {};
      state.presentation.importing.reset();
      append_completion(superseded, std::move(frame_done));
      frame_done = std::move(superseded);
    } else {
      pending->generation = state.presentation.lifetime->generation;
      if (pending->buffer) {
        if (auto *dma = static_cast<ImportedBuffer *>(
                wl_resource_get_user_data(pending->buffer))) {
          pending->region = state.committed.buffer_region(dma->width, dma->height);
          pending->buffer_scale = state.committed.buffer_scale;
          const bool stretch_now = state.waiting_for_resize_frame() &&
                                   can_stretch_to_target(state);
          pending->stretch_width = stretch_now ? resize_target_width(state) : 0;
          pending->stretch_height = stretch_now ? resize_target_height(state) : 0;
        }
      }
      append_completion(pending->on_presented, std::move(frame_done));
      frame_callback_submitted = static_cast<bool>(pending->on_presented);
    }
  }

  if (detaching_buffer) {
    cancel_resize_settle(state);
    detach_current_buffer(state);
    state.pending.attach.reset();
  } else if (attachment_requested && !attaching_buffer) {
    // The pending buffer resource was destroyed before commit. That
    // invalidates the attachment but is not wl_surface.attach(nullptr): keep
    // the committed image and discard only the invalid pending request.
    state.pending.attach.reset();
  }

  if (buffer_to_present && (state.is_subsurface() || state.is_popup()) &&
      !state.x11_map_requested) {
    backend.map_window(state.x11_window);
    state.x11_map_requested = true;
  }
  if (!buffer_to_present && state.content.has_size() && !detaching_buffer &&
      (buffer_scale_changed || geometry_updated || offset_updated || damaged)) {
    // The client's wl_buffer may already have been released and reused. Use
    // compositor-owned source pixels for SHM; use the backend's independent
    // retained pixmap/image for dmabuf and converted NV12. Never re-read the
    // client resource merely to apply crop or scale state.
    const auto region = state.committed.buffer_region(state.content.width,
                                            state.content.height);
    const auto logical_region = detail::scale_region(region, state.committed.buffer_scale);

    // wp_viewport: compute source crop region in buffer pixels
    // Viewport source is in surface-local coordinates; offset shifts the buffer
    // within the surface, so buffer coords = surface coords - offset.
    BufferRegion viewport_src_region{};
    bool has_viewport_src = false;
    if (!state.committed.viewport_src_unset) {
      const int32_t scale = std::max<int32_t>(state.committed.buffer_scale, 1);
      const int32_t src_x =
          (wl_fixed_to_int(state.committed.viewport_src_x) - state.committed.offset_x) *
          scale;
      const int32_t src_y =
          (wl_fixed_to_int(state.committed.viewport_src_y) - state.committed.offset_y) *
          scale;
      const int32_t src_w = wl_fixed_to_int(state.committed.viewport_src_width) * scale;
      const int32_t src_h = wl_fixed_to_int(state.committed.viewport_src_height) * scale;
      const int32_t x = std::clamp(src_x, 0, static_cast<int32_t>(state.content.width));
      const int32_t y = std::clamp(src_y, 0, static_cast<int32_t>(state.content.height));
      const int32_t w = std::min<int32_t>(src_w, static_cast<int32_t>(state.content.width) - x);
      const int32_t h = std::min<int32_t>(src_h, static_cast<int32_t>(state.content.height) - y);
      if (w > 0 && h > 0) {
        viewport_src_region = BufferRegion{
            static_cast<int16_t>(x), static_cast<int16_t>(y),
            static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
        has_viewport_src = true;
      }
    }

    // The region to present: viewport source crop takes precedence over window geometry
    const BufferRegion present_region = has_viewport_src ? viewport_src_region
                                : (region.declared() ? region : BufferRegion{0, 0, state.content.width, state.content.height});

    uint16_t committed_width = logical_region.declared()
                                         ? logical_region.width
                                         : detail::logical_size(
                                               state.content.width,
                                               state.committed.buffer_scale);
    uint16_t committed_height = logical_region.declared()
                                          ? logical_region.height
                                          : detail::logical_size(
                                                state.content.height,
                                                state.committed.buffer_scale);

    // wp_viewport: destination size overrides surface size
    if (!state.committed.viewport_dst_unset) {
      committed_width = static_cast<uint16_t>(state.committed.viewport_dst_width);
      committed_height = static_cast<uint16_t>(state.committed.viewport_dst_height);
    }
    // wp_viewport: source rectangle without destination -> cropping without scaling
    else if (!state.committed.viewport_src_unset) {
      const int32_t src_w = wl_fixed_to_int(state.committed.viewport_src_width);
      const int32_t src_h = wl_fixed_to_int(state.committed.viewport_src_height);
      if (src_w > 0 && src_h > 0) {
        committed_width = static_cast<uint16_t>(src_w);
        committed_height = static_cast<uint16_t>(src_h);
      }
    }

    const bool had_previous_commit = state.committed.content_size.has_value();
    const bool was_waiting_for_resize = state.waiting_for_resize_frame();
    const bool stale = update_committed_geometry(
        surface, state, committed_width, committed_height,
        had_previous_commit, was_waiting_for_resize);
    if (stale && !can_stretch_to_target(state))
      WW_LOG_WARN("[surface] refusing oversized stale-frame stretch "
                  "(target={}x{})", resize_target_width(state),
                  resize_target_height(state));
    if (const ShmContent *shm = state.content.retained_shm()) {
      if (stale && can_stretch_to_target(state)) {
        // Built from the compositor's own copy of the client's pixels, never
        // from an earlier stretched result.
        present_stretched_to_target(
            backend, state, shm->pixels.data(),
            shm->stride, state.content.width,
            state.content.height,
            present_region,
            frame_done);
      } else if (state.committed.buffer_scale == 1) {
        backend.present_pixels(state.x11_window, state.content.width,
                               state.content.height, shm->stride,
                               shm->pixels.data(), present_region,
                               frame_done);
      } else {
        auto scaled = detail::scale_shm_32(
            shm->pixels.data(), state.content.width,
            state.content.height, shm->stride,
            state.committed.buffer_scale);
        const BufferRegion scaled_present_region = has_viewport_src
            ? detail::scale_region(viewport_src_region, state.committed.buffer_scale)
            : detail::scale_region(region, state.committed.buffer_scale);
        backend.present_pixels(
            state.x11_window, scaled.width, scaled.height, scaled.stride,
            scaled.bytes.data(), scaled_present_region,
            frame_done);
      }
    } else if (stale && can_stretch_to_target(state)) {
      backend.repaint_window_scaled(state.x11_window, resize_target_width(state),
                                    resize_target_height(state), frame_done);
    } else {
      backend.repaint_window(state.x11_window, present_region, state.committed.buffer_scale,
                             frame_done);
    }
    frame_callback_submitted = static_cast<bool>(frame_done);
  }

  if (buffer_to_present) {
    wl_shm_buffer *shm = wl_shm_buffer_get(buffer_to_present);

    // libwayland only rejects stride < width, but every format we advertise
    // (ARGB8888 / XRGB8888) needs width * 4 bytes per row. A smaller stride
    // makes the downscale/copy paths below read past the end of the pool and
    // makes X reject the PutImage. Refuse it as a client error instead of
    // trusting the number. Compare against the raw int32 width: buf_w below
    // has already been clamped to 16 bits.
    bool stride_rejected = false;
    if (shm) {
      const int64_t min_stride =
          static_cast<int64_t>(wl_shm_buffer_get_width(shm)) * 4;
      const int32_t stride = wl_shm_buffer_get_stride(shm);
      if (stride < min_stride) {
        wl_resource_post_error(
            buffer_to_present, WL_SHM_ERROR_INVALID_STRIDE,
            "wl_shm buffer stride %d is smaller than width * 4 (%lld)",
            static_cast<int>(stride), static_cast<long long>(min_stride));
        WW_LOG_WARN("[surface] rejected wl_shm buffer with stride {} < {} "
                    "(surface={})",
                    stride, min_stride, static_cast<void *>(surface));
        shm = nullptr;
        stride_rejected = true; // fall through to the common tail below
      }
    }

    if (shm) {
      // Buffer dimensions come straight from the client and are int32_t.
      // A plain narrowing cast wraps: a width of 65536 truncates to 0, and
      // an X11 window may not be zero-sized, so the resize below would be
      // rejected by the server. Clamp into the representable window range
      // instead of trusting the client to stay inside it.
      const auto size = core::policy::clamp_size(wl_shm_buffer_get_width(shm),
                                                 wl_shm_buffer_get_height(shm));
      const uint16_t buf_w = size.width;
      const uint16_t buf_h = size.height;
      // Which part of that buffer is the window proper — everything else is
      // the client's own shadow and invisible resize border, and must not
      // reach the screen. Default (no xdg_surface.set_window_geometry) is
      // the whole buffer.
      const BufferRegion region = state.committed.buffer_region(buf_w, buf_h);
      const BufferRegion logical_region =
          detail::scale_region(region, state.committed.buffer_scale);
      uint16_t win_w = logical_region.declared()
                           ? logical_region.width
                           : detail::logical_size(buf_w, state.committed.buffer_scale);
      uint16_t win_h = logical_region.declared()
                           ? logical_region.height
                           : detail::logical_size(buf_h, state.committed.buffer_scale);

      // wp_viewport: destination size overrides surface size
      if (!state.committed.viewport_dst_unset) {
        win_w = static_cast<uint16_t>(state.committed.viewport_dst_width);
        win_h = static_cast<uint16_t>(state.committed.viewport_dst_height);
      }
      // wp_viewport: source rectangle without destination -> cropping without scaling
      else if (!state.committed.viewport_src_unset) {
        // Source rectangle is in fixed-point surface-local coordinates.
        // When destination is not set, src_width/height must be integers.
        const int32_t src_w = wl_fixed_to_int(state.committed.viewport_src_width);
        const int32_t src_h = wl_fixed_to_int(state.committed.viewport_src_height);
        if (src_w > 0 && src_h > 0) {
          win_w = static_cast<uint16_t>(src_w);
          win_h = static_cast<uint16_t>(src_h);
        }
      }

      // wp_viewport: compute source crop region in buffer pixels
      // The viewport source is in surface-local coordinates (after buffer_scale).
      // Offset shifts the buffer within the surface: buffer coords = surface coords - offset.
      BufferRegion viewport_src_region{};
      bool has_viewport_src = false;
      if (!state.committed.viewport_src_unset) {
        const int32_t scale = std::max<int32_t>(state.committed.buffer_scale, 1);
        const int32_t src_x =
            (wl_fixed_to_int(state.committed.viewport_src_x) - state.committed.offset_x) *
            scale;
        const int32_t src_y =
            (wl_fixed_to_int(state.committed.viewport_src_y) - state.committed.offset_y) *
            scale;
        const int32_t src_w = wl_fixed_to_int(state.committed.viewport_src_width) * scale;
        const int32_t src_h = wl_fixed_to_int(state.committed.viewport_src_height) * scale;
        // Clamp to buffer bounds
        const int32_t x = std::clamp(src_x, 0, static_cast<int32_t>(buf_w));
        const int32_t y = std::clamp(src_y, 0, static_cast<int32_t>(buf_h));
        const int32_t w = std::min<int32_t>(src_w, static_cast<int32_t>(buf_w) - x);
        const int32_t h = std::min<int32_t>(src_h, static_cast<int32_t>(buf_h) - y);
        if (w > 0 && h > 0) {
          viewport_src_region = BufferRegion{
              static_cast<int16_t>(x), static_cast<int16_t>(y),
              static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
          has_viewport_src = true;
        }
      }

      const bool had_previous_commit = state.committed.content_size.has_value();
      const bool was_waiting_for_resize = state.waiting_for_resize_frame();
      bool stale = false;
      // Subsurfaces and popups have no toplevel configure handshake: their
      // child X11 window follows their committed geometry directly.
      if (state.is_subsurface() || state.is_popup()) {
        state.committed.content_size = policy::WindowSize{win_w, win_h};
        state.stop_waiting_for_resize_frame();
        backend.resize_window(state.x11_window, win_w, win_h);
      } else {
        stale = update_committed_geometry(surface, state, win_w, win_h,
                                          had_previous_commit,
                                          was_waiting_for_resize);
        if (stale && !can_stretch_to_target(state))
          WW_LOG_WARN("[surface] refusing oversized stale-frame stretch "
                      "(target={}x{})", resize_target_width(state),
                      resize_target_height(state));
      }

      wl_shm_buffer_begin_access(shm);
      const auto *raw_pixels = wl_shm_buffer_get_data(shm);
      const uint32_t raw_stride =
          static_cast<uint32_t>(wl_shm_buffer_get_stride(shm));
      // X's PutImage takes the row pitch in whole pixels, so a stride that is
      // not a multiple of 4 bytes (legal for wl_shm, as long as it is at least
      // width * 4 -- checked above) would be truncated and the image would
      // shear. Repack such buffers into a tight copy once, here, so everything
      // downstream (retained copy, downscale, X put) sees a valid pitch.
      std::vector<uint8_t> repacked;
      const void *source_pixels = raw_pixels;
      uint32_t source_stride = raw_stride;
      if (raw_stride % 4u != 0u) {
        const size_t row_bytes = static_cast<size_t>(buf_w) * 4u;
        repacked.resize(row_bytes * buf_h);
        const auto *rows = static_cast<const uint8_t *>(raw_pixels);
        for (size_t y = 0; y < buf_h; ++y)
          std::memcpy(repacked.data() + y * row_bytes,
                      rows + y * static_cast<size_t>(raw_stride), row_bytes);
        source_pixels = repacked.data();
        source_stride = static_cast<uint32_t>(row_bytes);
      }
      const size_t retained_size =
          static_cast<size_t>(source_stride) * static_cast<size_t>(buf_h);
      ShmContent retained;
      retained.stride = source_stride;
      retained.pixels.resize(retained_size);
      if (retained_size != 0)
        std::memcpy(retained.pixels.data(), source_pixels, retained_size);
      state.content.storage = std::move(retained);
      // The region to present: viewport source crop takes precedence over window geometry
      const BufferRegion present_region = has_viewport_src ? viewport_src_region
                                 : (region.declared() ? region : BufferRegion{0, 0, buf_w, buf_h});
      if (stale && can_stretch_to_target(state)) {
        present_stretched_to_target(
            backend, state, source_pixels, source_stride, buf_w, buf_h,
            present_region, frame_done);
        frame_callback_submitted = static_cast<bool>(frame_done);
      } else if (state.committed.buffer_scale == 1) {
        backend.present_pixels(state.x11_window, buf_w, buf_h, source_stride,
                               source_pixels, present_region, frame_done);
        frame_callback_submitted = static_cast<bool>(frame_done);
      } else {
        auto scaled = detail::scale_shm_32(source_pixels, buf_w, buf_h,
                                           source_stride, state.committed.buffer_scale);
        // For scaled presentation, the logical_region needs to be adjusted for viewport source
        const BufferRegion scaled_present_region = has_viewport_src
            ? detail::scale_region(viewport_src_region, state.committed.buffer_scale)
            : logical_region;
        backend.present_pixels(state.x11_window, scaled.width, scaled.height,
                               scaled.stride, scaled.bytes.data(), scaled_present_region,
                               frame_done);
        frame_callback_submitted = static_cast<bool>(frame_done);
      }
      wl_shm_buffer_end_access(shm);

    } else if (!stride_rejected) {
      state.content.storage = DmabufContent{};
      auto *dma = static_cast<ImportedBuffer *>(
          wl_resource_get_user_data(buffer_to_present));
      if (dma) {
        const auto size = core::policy::clamp_size(dma->width, dma->height);
        const BufferRegion region = state.committed.buffer_region(size.width,
                                                        size.height);
        const BufferRegion logical_region =
            detail::scale_region(region, state.committed.buffer_scale);
        uint16_t win_w = logical_region.declared()
                                   ? logical_region.width
                                   : detail::logical_size(size.width,
                                                          state.committed.buffer_scale);
        uint16_t win_h = logical_region.declared()
                                   ? logical_region.height
                                   : detail::logical_size(size.height,
                                                          state.committed.buffer_scale);

        // wp_viewport: destination size overrides surface size
        if (!state.committed.viewport_dst_unset) {
          win_w = static_cast<uint16_t>(state.committed.viewport_dst_width);
          win_h = static_cast<uint16_t>(state.committed.viewport_dst_height);
        }
        // wp_viewport: source rectangle without destination -> cropping without scaling
        else if (!state.committed.viewport_src_unset) {
          const int32_t src_w = wl_fixed_to_int(state.committed.viewport_src_width);
          const int32_t src_h = wl_fixed_to_int(state.committed.viewport_src_height);
          if (src_w > 0 && src_h > 0) {
            win_w = static_cast<uint16_t>(src_w);
            win_h = static_cast<uint16_t>(src_h);
          }
        }

        // wp_viewport: compute source crop region in buffer pixels
        // Viewport source is in surface-local coordinates; offset shifts the buffer
        // within the surface, so buffer coords = surface coords - offset.
        BufferRegion viewport_src_region{};
        bool has_viewport_src = false;
        if (!state.committed.viewport_src_unset) {
          const int32_t scale = std::max<int32_t>(state.committed.buffer_scale, 1);
          const int32_t src_x =
              (wl_fixed_to_int(state.committed.viewport_src_x) - state.committed.offset_x) *
              scale;
          const int32_t src_y =
              (wl_fixed_to_int(state.committed.viewport_src_y) - state.committed.offset_y) *
              scale;
          const int32_t src_w = wl_fixed_to_int(state.committed.viewport_src_width) * scale;
          const int32_t src_h = wl_fixed_to_int(state.committed.viewport_src_height) * scale;
          const int32_t x = std::clamp(src_x, 0, static_cast<int32_t>(size.width));
          const int32_t y = std::clamp(src_y, 0, static_cast<int32_t>(size.height));
          const int32_t w = std::min<int32_t>(src_w, static_cast<int32_t>(size.width) - x);
          const int32_t h = std::min<int32_t>(src_h, static_cast<int32_t>(size.height) - y);
          if (w > 0 && h > 0) {
            viewport_src_region = BufferRegion{
                static_cast<int16_t>(x), static_cast<int16_t>(y),
                static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
            has_viewport_src = true;
          }
        }

        const bool had_previous_commit = state.committed.content_size.has_value();
        const bool was_waiting_for_resize = state.waiting_for_resize_frame();
        bool stale = false;
        if (state.is_subsurface() || state.is_popup()) {
          state.committed.content_size = policy::WindowSize{win_w, win_h};
          state.stop_waiting_for_resize_frame();
          backend.resize_window(state.x11_window, win_w, win_h);
        } else {
          stale = update_committed_geometry(surface, state, win_w, win_h,
                                            had_previous_commit,
                                            was_waiting_for_resize);
          if (stale && !can_stretch_to_target(state))
            WW_LOG_WARN("[surface] refusing oversized stale-frame stretch "
                        "(target={}x{})", resize_target_width(state),
                        resize_target_height(state));
        }
        const uint16_t stretch_width = stale && can_stretch_to_target(state)
                                           ? resize_target_width(state) : 0;
        const uint16_t stretch_height = stale && can_stretch_to_target(state)
                                            ? resize_target_height(state) : 0;

        // The region to present: viewport source crop takes precedence over window geometry
        const BufferRegion present_region = has_viewport_src ? viewport_src_region : region;

        if (dma->format == ImportedBufferFormat::Nv12) {
          backend.present_nv12(
              state.x11_window, dma->width, dma->height,
              dma->planes[0].fd, dma->planes[0].offset,
              dma->planes[0].stride, dma->planes[1].fd,
              dma->planes[1].offset, dma->planes[1].stride, present_region,
              state.committed.buffer_scale, frame_done, stretch_width, stretch_height);
          frame_callback_submitted = static_cast<bool>(frame_done);
        } else if (dma->pixmap) {
          // Already imported by an earlier commit of this same buffer
          // object (clients commonly recommit the same dmabuf across
          // frames) — present immediately, no round-trip needed.
          WW_LOG_TRACE("[surface] presenting dmabuf pixmap {} on x11 win {}",
                       dma->pixmap, state.x11_window);
          auto source_consumed =
              track_dmabuf_source_read(*dma, buffer_to_present);
          backend.present_pixmap(state.x11_window, dma->pixmap, present_region,
                                 state.committed.buffer_scale, frame_done,
                                 std::move(source_consumed), stretch_width,
                                 stretch_height);
          frame_callback_submitted = static_cast<bool>(frame_done);
        } else if (dma->fd >= 0) {
          const int fd = dma->fd;
          // Never retry: success or failure, this buffer object gets one
          // import attempt, matching what happens on the success path
          // below (dma->pixmap becomes non-zero and every future commit
          // of this object takes the branch above instead).
          dma->fd = -1;
          dma->import_status = DmabufImportStatus::Pending;
          IWindowBackend *backend_ptr = &backend;
          const uint32_t win = state.x11_window;
          std::weak_ptr<bool> alive = dma->alive;
          std::weak_ptr<SurfacePresentationLifetime> surface_lifetime =
              state.presentation.lifetime;
          const uint64_t generation = state.presentation.lifetime->generation;
          auto pending_presentation =
              std::make_shared<ImportInFlight>();
          pending_presentation->buffer = buffer_to_present;
          pending_presentation->generation = generation;
          pending_presentation->region = present_region;
          pending_presentation->buffer_scale = state.committed.buffer_scale;
          pending_presentation->stretch_width = stretch_width;
          pending_presentation->stretch_height = stretch_height;
          pending_presentation->on_presented = std::move(frame_done);
          pending_presentation->on_source_consumed =
              track_dmabuf_source_read(*dma, buffer_to_present);
          state.presentation.importing = pending_presentation;
          Impl *const impl_ptr = this;
          WW_LOG_TRACE("[surface] importing dmabuf for x11 win {} (surface={})",
                       win, static_cast<void *>(surface));
          backend_ptr->import_dmabuf(
              fd, win, dma->width, dma->height,
              static_cast<uint8_t>(dma->depth), static_cast<uint8_t>(dma->bpp),
              static_cast<uint16_t>(dma->stride), dma->modifier,
              dma->bgr_order,
              [dma, backend_ptr, win, alive, surface_lifetime, surface,
               impl_ptr, pending_presentation](uint32_t pixmap) mutable {
                auto buffer_alive = alive.lock();
                if (!buffer_alive || !*buffer_alive) {
                  // The wl_buffer was destroyed before the import resolved.
                  // Discard a late pixmap and release the source-read hold;
                  // its weak resource token prevents sending an event to a
                  // dead Wayland resource.
                  if (pixmap)
                    backend_ptr->free_pixmap(pixmap);
                  if (pending_presentation->on_source_consumed)
                    pending_presentation->on_source_consumed();
                  return;
                }
                dma->pixmap = pixmap;
                dma->import_status = pixmap ? DmabufImportStatus::NotStarted
                                            : DmabufImportStatus::Failed;

                auto lifetime = surface_lifetime.lock();
                if (!lifetime || !lifetime->alive ||
                    lifetime->generation != pending_presentation->generation) {
                  if (pending_presentation->on_source_consumed)
                    pending_presentation->on_source_consumed();
                  return;
                }

                // Check the weak surface lifetime before dereferencing Impl:
                // a surface may have been destroyed while the import was in
                // flight. The generation protects against an old import
                // overwriting a newer committed frame on a still-live surface.
                auto surface_it = impl_ptr->surfaces.find(surface);
                if (surface_it == impl_ptr->surfaces.end() ||
                    surface_it->second.presentation.lifetime != lifetime ||
                    surface_it->second.presentation.importing !=
                        pending_presentation) {
                  if (pending_presentation->on_source_consumed)
                    pending_presentation->on_source_consumed();
                  return;
                }
                surface_it->second.presentation.importing.reset();

                if (pixmap) {
                  WW_LOG_TRACE(
                      "[surface] presenting current dmabuf pixmap {} on x11 "
                      "win {} (import completed)", pixmap, win);
                  backend_ptr->present_pixmap(
                      win, pixmap, pending_presentation->region,
                      pending_presentation->buffer_scale,
                      std::move(pending_presentation->on_presented),
                      std::move(pending_presentation->on_source_consumed),
                      pending_presentation->stretch_width,
                      pending_presentation->stretch_height);
                } else {
                  WW_LOG_WARN(
                      "[surface] dmabuf import failed asynchronously (x11={})",
                      win);
                  if (pending_presentation->on_source_consumed)
                    pending_presentation->on_source_consumed();
                  if (pending_presentation->on_presented)
                    pending_presentation->on_presented();
                }
              });
          frame_callback_submitted =
              static_cast<bool>(pending_presentation->on_presented);
        } else if (dma->import_status == DmabufImportStatus::Pending) {
          // The import fired on an earlier commit and hasn't come back yet.
          // Benign and self-correcting: that import's own callback presents
          // the pixmap when it resolves. Nothing to warn about.
          WW_LOG_TRACE(
              "[surface] commit while dmabuf import still in flight "
              "(surface={}, x11={})",
              static_cast<void *>(surface), state.x11_window);
        } else {
          WW_LOG_WARN(
              "[surface] dmabuf commit has no presentable pixmap (surface={}, "
              "import={})",
              static_cast<void *>(surface),
              dma->import_status == DmabufImportStatus::Failed
                  ? "failed"
                  : "never started");
        }
      } else {
        WW_LOG_WARN(
            "[surface] buffer is neither wl_shm nor dmabuf (surface={}, buffer={})",
            static_cast<void *>(surface),
            static_cast<void *>(buffer_to_present));
      }
    }

    if (attaching_buffer)
      retire_pending_buffer(state);
  }

  if (!frame_callback_submitted && frame_done)
    frame_done();

  WW_LOG_TRACE("[surface] commit done (surface={}, x11={})",
               static_cast<void *>(surface), state.x11_window);
}

void SurfaceManager::Impl::retire_pending_buffer(Surface &state) {
  wl_resource *const pending_buffer = state.pending.attached_buffer();
  wl_list_remove(&state.destroy_listener.link);
  wl_list_init(&state.destroy_listener.link);

  if (state.last_buffer)
  {
    wl_list_remove(&state.last_buffer_destroy_listener.link);
    wl_list_init(&state.last_buffer_destroy_listener.link);
  }

  bool defer_release = false;
  // wl_shm buffers carry a wl_shm_buffer* as resource user data, not an
  // ImportedBuffer*, so the shm check must come first: only reinterpret the
  // user data as an ImportedBuffer once it is known not to be shm.
  if (wl_shm_buffer *shm = wl_shm_buffer_get(pending_buffer)) {
    const auto size = core::policy::clamp_size(
        wl_shm_buffer_get_width(shm), wl_shm_buffer_get_height(shm));
    state.content.width = size.width;
    state.content.height = size.height;
    // The present step in commit() has normally already retained this frame's
    // pixels. Keep them only if they are exactly this buffer's size; anything
    // else is an earlier frame's and must not pair with the new size.
    const auto *kept = std::get_if<ShmContent>(&state.content.storage);
    if (!kept || kept->pixels.size() !=
                     static_cast<std::size_t>(kept->stride) * size.height)
      state.content.storage = ShmContent{};
  } else if (auto *dma = static_cast<ImportedBuffer *>(
                 wl_resource_get_user_data(pending_buffer));
             dma && dma->use_state) {
    if (dma->use_state->source_reads_in_flight > 0) {
      dma->use_state->release_pending = true;
      defer_release = true;
    }
    state.content.width = dma->width;
    state.content.height = dma->height;
    state.content.storage = DmabufContent{};
  } else {
    state.content.clear();
  }
  if (!defer_release) {
    wl_buffer_send_release(pending_buffer);
    ::ww::diag::note(state.x11_window, ::ww::diag::Ev::Release, 2);
  }
  state.last_buffer = pending_buffer;

  state.last_buffer_destroy_listener.notify = Impl::on_last_buffer_destroyed;
  wl_resource_add_destroy_listener(state.last_buffer,
                                   &state.last_buffer_destroy_listener);

  state.pending.attach.reset();
}

bool SurfaceManager::Impl::rebuild_cursor(Surface &state,
                                          wl_resource *buffer) {
  Cursor *cursor_ptr = state.as_cursor();
  if (!cursor_ptr)
    return false; // only a surface holding the cursor role owns a cursor
  Cursor &cursor_role = *cursor_ptr;
  wl_shm_buffer *shm = buffer ? wl_shm_buffer_get(buffer) : nullptr;
  if (!shm) {
    // Cursor themes are always wl_shm (libwayland-cursor, GTK, Qt, Firefox's
    // custom CSS cursors). A dmabuf cursor would need a GPU readback for a
    // 24x24 image -- not worth supporting until a client actually does it.
    WW_LOG_DEBUG("[surface] cursor buffer is not wl_shm — keeping the "
                 "previous cursor");
    return false;
  }

  const uint32_t format = wl_shm_buffer_get_format(shm);
  if (format != WL_SHM_FORMAT_ARGB8888 && format != WL_SHM_FORMAT_XRGB8888) {
    WW_LOG_DEBUG("[surface] cursor buffer has unsupported wl_shm format 0x{:x}",
                 format);
    return false;
  }

  const auto size = core::policy::clamp_size(wl_shm_buffer_get_width(shm),
                                             wl_shm_buffer_get_height(shm));
  const int32_t scale = std::max<int32_t>(state.committed.buffer_scale, 1);

  wl_shm_buffer_begin_access(shm);
  const void *pixels = wl_shm_buffer_get_data(shm);
  const uint32_t stride = static_cast<uint32_t>(wl_shm_buffer_get_stride(shm));

  // The host X server has no notion of buffer scale: its pointer is always
  // 1:1 with logical pixels, same as every other surface this compositor
  // presents (see the scale_shm_32 call in apply_surface_state). A cursor
  // drawn for scale 2 is therefore downsampled to its logical size, with the
  // hotspot (already surface-local, i.e. logical) left as it is.
  detail::ScaledPixels scaled;
  CursorImage image{.width = size.width,
                    .height = size.height,
                    .hotspot_x = static_cast<int16_t>(std::clamp<int32_t>(
                        cursor_role.hotspot_x, INT16_MIN, INT16_MAX)),
                    .hotspot_y = static_cast<int16_t>(std::clamp<int32_t>(
                        cursor_role.hotspot_y, INT16_MIN, INT16_MAX)),
                    .stride = stride,
                    .argb = pixels};
  if (scale > 1) {
    scaled = detail::scale_shm_32(pixels, size.width, size.height, stride,
                                  scale);
    image.width = scaled.width;
    image.height = scaled.height;
    image.stride = scaled.stride;
    image.argb = scaled.bytes.data();
  }

  // XRGB8888's top byte is padding with no promised value; as ARGB it would
  // be read as alpha and could make the whole cursor transparent. Force it
  // opaque on a private copy -- the client's buffer is read-only to us.
  std::vector<uint8_t> opaque;
  if (format == WL_SHM_FORMAT_XRGB8888) {
    const std::size_t row_bytes = static_cast<std::size_t>(image.width) * 4u;
    opaque.resize(row_bytes * image.height);
    const auto *src = static_cast<const uint8_t *>(image.argb);
    for (uint16_t y = 0; y < image.height; ++y) {
      uint8_t *dst = opaque.data() + static_cast<std::size_t>(y) * row_bytes;
      std::memcpy(dst, src + static_cast<std::size_t>(y) * image.stride,
                  row_bytes);
      for (std::size_t x = 3; x < row_bytes; x += 4)
        dst[x] = 0xff;
    }
    image.argb = opaque.data();
    image.stride = static_cast<uint32_t>(row_bytes);
  }

  const uint32_t cursor = backend.create_cursor(image);
  wl_shm_buffer_end_access(shm);

  if (!cursor)
    return false;

  // The old cursor may still be showing on the window under the pointer;
  // freeing it is safe regardless (see IWindowBackend::create_cursor), and
  // the caller applies the new one right away.
  if (cursor_role.handle)
    backend.free_cursor(cursor_role.handle);
  cursor_role.handle = cursor;
  return true;
}

void SurfaceManager::Impl::apply_cursor_surface_state(wl_resource *surface,
                                                      Surface &state) {
  // The wl_surface.offset that moves a cursor's content moves its hotspot the
  // opposite way: the hotspot stays pinned to the same *pixel* of the image.
  const int32_t dx = state.pending.offset_x;
  const int32_t dy = state.pending.offset_y;
  state.committed.offset_x += dx;
  state.committed.offset_y += dy;
  state.pending.offset_x = 0;
  state.pending.offset_y = 0;
  Cursor &cursor_role = *state.as_cursor();
  cursor_role.hotspot_x -= dx;
  cursor_role.hotspot_y -= dy;

  if (state.pending.buffer_scale) {
    state.committed.buffer_scale = std::max<int32_t>(*state.pending.buffer_scale, 1);
    state.pending.buffer_scale.reset();
  }

  wl_resource *const pending_buffer = state.pending.attached_buffer();
  const bool attachment_requested = state.pending.attach.has_value();
  const bool attaching = pending_buffer != nullptr;
  bool changed = false;
  if (attaching) {
    changed = rebuild_cursor(state, pending_buffer);
    // Consumed either way: a cursor buffer the server could not use must not
    // sit pending and be retried on every later commit.
    retire_pending_buffer(state);
  } else if (state.pending.detach_requested()) {
    // Explicit wl_surface.attach(nullptr): a cursor surface is detached, so
    // stop showing its old image and release the retained source reference.
    if (state.last_buffer) {
      wl_list_remove(&state.last_buffer_destroy_listener.link);
      wl_list_init(&state.last_buffer_destroy_listener.link);
      state.last_buffer = nullptr;
    }
    state.pending.attach.reset();
    if (cursor_role.handle) {
      backend.free_cursor(cursor_role.handle);
      cursor_role.handle = 0;
      changed = true;
    }
  } else if (attachment_requested) {
    // The pending buffer resource was destroyed before commit. That isn't an
    // explicit detach; preserve the current cursor and discard the invalid
    // pending attachment.
    state.pending.attach.reset();
  } else if ((dx != 0 || dy != 0) && state.last_buffer) {
    // No new content, but the hotspot moved and it is part of the X cursor.
    changed = rebuild_cursor(state, state.last_buffer);
  }

  // Animated cursors pace themselves with frame callbacks; nothing here is
  // ever presented, so they complete immediately.
  auto callbacks = std::move(state.frame_callbacks);
  state.frame_callbacks.clear();
  if (auto frame_done = detail::make_frame_completion(
          std::move(callbacks), state.frame_callbacks_alive, state.x11_window))
    frame_done();

  WW_LOG_TRACE("[surface] cursor commit (surface={}, new_cursor={})",
               static_cast<void *>(surface), changed);

  if (changed && cursor_role.changed)
    cursor_role.changed();
}

} // namespace ww
