#include "wwindows/core/surface_manager.hpp"
#include "wwindows/core/detail/surface_manager_impl.hpp"
#include <wwindows/core/detail/pixel_scale.hpp>

#include <wwindows/support/log.hpp>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <sys/types.h>
#include <vector>
#include <wayland-server-core.h>
#include <wwindows/adapters/wayland/protocols/xdg-shell-server-protocol.h>

namespace ww {

namespace {

// The xdg_toplevel states to advertise for a surface, in the fixed order
// the dedup comparison below relies on.
//
// These are not decoration: a client uses them to decide whether to draw a
// drop shadow and rounded corners at all. A toplevel that believes it is
// floating draws both, which means a buffer bigger than the configure it
// was given and a non-zero window-geometry offset — so telling the truth
// here is what makes the common case need no geometry crop in the first
// place.
std::vector<uint32_t> toplevel_states(const Toplevel &t) {
  std::vector<uint32_t> states;
  // Reported from what the window manager has actually done, never from what
  // the client asked for -- see Toplevel::wm.
  if (t.wm.fullscreen)
    states.push_back(XDG_TOPLEVEL_STATE_FULLSCREEN);
  if (t.wm_maximized_both_axes())
    states.push_back(XDG_TOPLEVEL_STATE_MAXIMIZED);
  if (t.activated)
    states.push_back(XDG_TOPLEVEL_STATE_ACTIVATED);

  // Tiled means "a neighbour or the screen edge is on every side of you" --
  // how a tiling window manager lays a window out, per edge (see
  // ww::TiledEdges' own comment for why, for the one watcher that can
  // currently tell edges apart at all, this is in practice always every
  // edge or none). A fullscreen window is by definition not part of the
  // tiling, and the TILED_* states only exist from xdg_toplevel version 2;
  // an older client would not know what to do with them.
  if (t.toplevel &&
      wl_resource_get_version(t.toplevel) >=
          XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION) {
    const TiledEdges edges = t.effective_tiled(); // empty when fullscreen
    if (edges.left)
      states.push_back(XDG_TOPLEVEL_STATE_TILED_LEFT);
    if (edges.right)
      states.push_back(XDG_TOPLEVEL_STATE_TILED_RIGHT);
    if (edges.top)
      states.push_back(XDG_TOPLEVEL_STATE_TILED_TOP);
    if (edges.bottom)
      states.push_back(XDG_TOPLEVEL_STATE_TILED_BOTTOM);
  }
  return states;
}

// The limits that will be active after the next commit. The first min/max
// request since the last commit starts from the committed limits, so setting
// one bound never resets the other.
WindowSizeLimits &pending_size_limits(Surface &state) {
  if (!state.pending.size_limits)
    state.pending.size_limits = state.committed.size_limits;
  return *state.pending.size_limits;
}

void send_configure(Surface &state, uint16_t width, uint16_t height,
                    std::vector<uint32_t> states) {
  // Configure serials share the display's serial sequence with input events
  // and all other protocol traffic. A private counter can collide with serials
  // emitted by wl_display_next_serial() in popup handlers and other globals.
  Toplevel &t = *state.live_toplevel(); // callers check liveness
  wl_client *client = wl_resource_get_client(t.xdg_surface);
  const uint32_t serial =
      wl_display_next_serial(wl_client_get_display(client));
  wl_array arr;
  wl_array_init(&arr);
  for (uint32_t s : states) {
    if (void *slot = wl_array_add(&arr, sizeof(uint32_t)))
      *static_cast<uint32_t *>(slot) = s;
  }
  xdg_toplevel_send_configure(t.toplevel, width, height, &arr);
  wl_array_release(&arr);

  xdg_surface_send_configure(t.xdg_surface, serial);

  // The acknowledgement belongs to this surface's protocol state, not to
  // the request path that happened to emit the configure.
  state.configure_log.sent.push_back(serial);
  constexpr std::size_t kConfigureHistoryLimit = 4096;
  if (state.configure_log.sent.size() > kConfigureHistoryLimit)
    state.configure_log.sent.pop_front();

  t.configured = policy::ConfiguredState{
      policy::WindowSize{width, height}, std::move(states)};
}

} // namespace

// The single gate every configure after the initial one goes through, so
// there is exactly one place that decides whether the client needs to hear
// about a change.
//
// One window-manager decision usually produces several ConfigureNotify
// events (reparent, then fit-to-workarea), and X11Backend forwards each
// verbatim. Re-sending a configure the client already has costs it a full
// redraw and burns a configure serial for nothing.
//
// The comparison covers size *and* states, which is what makes it safe to
// route state changes through here too: maximizing a toplevel that is
// already the size it will be when maximized changes no dimension at all,
// and on size alone that configure would be dropped — leaving the client
// convinced it is still floating, and still drawing a shadow it no longer
// has room for.
bool SurfaceManager::Impl::configure_if_changed(Surface &state,
                                                uint16_t width,
                                                uint16_t height) {
  Toplevel *const t = state.live_toplevel();
  if (!t)
    return false;

  auto states = toplevel_states(*t);
  if (!policy::configure_needed(t->configured,
                                policy::WindowSize{width, height}, states))
    return false;

  send_configure(state, width, height, std::move(states));
  return true;
}

namespace {
constexpr auto kResizeSettleDelay = std::chrono::milliseconds{250};
constexpr auto kResizeEchoLifetime = std::chrono::seconds{1};

bool wm_owns_geometry(const Toplevel &t) {
  // Observed WM state, or a request the WM may be about to act on. Either
  // maximize axis counts: a partial maximize / edge snap is still WM-owned.
  return t.wants_fullscreen || t.wants_maximized || t.wm.fullscreen ||
         t.wm.maximized_horz || t.wm.maximized_vert ||
         t.effective_tiled().any();
}

bool wm_owns_geometry(const Surface &state) {
  const Toplevel *t = state.as_toplevel();
  return t && wm_owns_geometry(*t);
}

// A settle timer may resize the X11 window without a new client commit. The
// backend could still retain the interim stretched frame, so immediately
// repaint the most recent commit at its committed geometry after adoption.
// SHM has a compositor-owned canonical copy here; dmabuf and NV12 replay their
// own retained source through the backend's native presentation path.
void repaint_committed_frame(IWindowBackend &backend,
                             const Surface &state) {
  if (!state.x11_window)
    return;
  const ShmContent *shm = state.content.retained_shm();
  if (!shm || !state.content.has_size()) {
    backend.repaint_window(state.x11_window);
    return;
  }

  const BufferRegion region = state.committed.buffer_region(state.content.width,
                                                  state.content.height);
  if (state.committed.buffer_scale <= 1) {
    backend.present_pixels(state.x11_window, state.content.width,
                           state.content.height, shm->stride,
                           shm->pixels.data(), region, {});
    return;
  }
  const auto scaled = detail::scale_shm_32(
      shm->pixels.data(), state.content.width, state.content.height,
      shm->stride, state.committed.buffer_scale);
  backend.present_pixels(state.x11_window, scaled.width, scaled.height,
                         scaled.stride, scaled.bytes.data(),
                         detail::scale_region(region, state.committed.buffer_scale), {});
}
} // namespace

void SurfaceManager::Impl::cancel_resize_settle(Surface &state) noexcept {
  Toplevel *const t = state.as_toplevel();
  if (!t)
    return;
  if (resize_timer && t->resize_settle_timer)
    resize_timer->cancel(t->resize_settle_timer);
  t->resize_settle_timer = 0;
}

void SurfaceManager::Impl::remember_resize_echo(Surface &state,
                                                uint16_t width,
                                                uint16_t height) {
  Toplevel *const t = state.as_toplevel();
  if (!t)
    return;
  const auto now = resize_timer ? resize_timer->now()
                                : IResizeTimer::Clock::now();
  std::erase_if(t->expected_resize_echoes,
                [now](const ExpectedResizeEcho &echo) {
                  return echo.expires_at <= now;
                });
  const auto expires_at = now + kResizeEchoLifetime;
  const auto existing = std::find_if(
      t->expected_resize_echoes.begin(), t->expected_resize_echoes.end(),
      [width, height](const ExpectedResizeEcho &echo) {
        return echo.width == width && echo.height == height;
      });
  if (existing != t->expected_resize_echoes.end()) {
    existing->expires_at = expires_at;
    return;
  }
  t->expected_resize_echoes.push_back(
      ExpectedResizeEcho{width, height, expires_at});
}

void SurfaceManager::Impl::schedule_resize_settle(wl_resource *surface,
                                                  Surface &state) {
  Toplevel *const t = state.live_toplevel();
  if (!resize_timer || !surface || !t || !t->wm_target || !t->wm_target->observed ||
      wm_owns_geometry(state) || t->wm_target->overridden ||
      t->wm_target->size.empty()) {
    cancel_resize_settle(state);
    return;
  }

  if (state.committed.content_size && t->x11_size &&
      *state.committed.content_size == *t->x11_size) {
    cancel_resize_settle(state);
    t->waiting_for_resize_frame = false;
    return;
  }

  if (state.committed.content_size && *state.committed.content_size == t->wm_target->size) {
    cancel_resize_settle(state);
    t->waiting_for_resize_frame = false;
    return;
  }

  if (t->resize_settle_timer)
    return;

  const uint64_t generation = t->wm_target->generation;
  const auto deadline = t->wm_target->since + kResizeSettleDelay;
  t->resize_settle_timer = resize_timer->schedule_at(
      deadline, [this, surface, generation] {
        const auto it = surfaces.find(surface);
        if (it == surfaces.end())
          return;
        if (auto *top = it->second.as_toplevel())
          top->resize_settle_timer = 0;
        on_resize_settle_timeout(surface, generation);
      });
}

void SurfaceManager::Impl::on_resize_settle_timeout(
    wl_resource *surface, uint64_t target_generation) {
  const auto it = surfaces.find(surface);
  if (it == surfaces.end())
    return;
  Surface &state = it->second;
  Toplevel *const t = state.live_toplevel();
  if (!t || !t->wm_target || target_generation != t->wm_target->generation ||
      !t->wm_target->observed ||
      wm_owns_geometry(state) || t->wm_target->overridden)
    return;

  // A timer firing is not sufficient by itself: every outstanding configure
  // must have been acknowledged and a committed geometry must exist. Evaluate
  // that latest committed geometry directly; no further commit is required
  // after the deadline, which matters for idle grid-quantized clients.
  if (!state.configure_log.last_acked ||
      !state.configure_log.sent.empty() || !state.committed.content_size) {
    return;
  }

  if (resize_timer &&
      resize_timer->now() < t->wm_target->since + kResizeSettleDelay) {
    schedule_resize_settle(surface, state);
    return;
  }

  const policy::WindowSize committed = *state.committed.content_size;
  if (t->wm_target->size == committed) {
    t->waiting_for_resize_frame = false;
    return;
  }
  if (committed.empty())
    return;

  adopt_committed_geometry(surface, state, committed.width, committed.height);
  repaint_committed_frame(backend, state);
}

void SurfaceManager::Impl::adopt_committed_geometry(wl_resource *surface,
                                                    Surface &state,
                                                    uint16_t width,
                                                    uint16_t height) {
  (void)surface;
  Toplevel *const t = state.as_toplevel();
  if (!t || !state.x11_window || width == 0 || height == 0)
    return;

  // Do not mutate Toplevel::configured: it describes
  // the latest Wayland configure that was sent, not the X11 window's geometry.
  // Remember the resize's expected ConfigureNotify echo so it cannot be
  // mistaken for a fresh WM target or trigger a redundant configure.
  remember_resize_echo(state, width, height);
  t->x11_size = policy::WindowSize{width, height};
  t->waiting_for_resize_frame = false;
  if (t->wm_target)
    t->wm_target->adopted = true;
  if (state.presentation.importing) {
    state.presentation.importing->stretch_width = 0;
    state.presentation.importing->stretch_height = 0;
  }
  cancel_resize_settle(state);
  backend.resize_window(state.x11_window, width, height);
  WW_LOG_INFO("[surface] adopted committed client geometry (x11={}, size={}x{}, "
              "wm_target={}x{})",
              state.x11_window, width, height, state.resize_target().width,
              state.resize_target().height);
}


bool SurfaceManager::Impl::update_committed_geometry(
    wl_resource *surface, Surface &state, uint16_t width,
    uint16_t height, bool had_previous_commit, bool was_waiting_for_resize) {
  const policy::WindowSize committed{width, height};
  state.committed.content_size = committed.empty()
                             ? std::nullopt
                             : std::optional<policy::WindowSize>{committed};

  Toplevel *const t = state.live_toplevel();
  if (!t || !t->configured) {
    state.stop_waiting_for_resize_frame();
    cancel_resize_settle(state);
    return false;
  }

  // The decision is pure (policy::classify_commit); only its effects live here.
  switch (policy::classify_commit(policy::CommitInputs{
      committed, t->x11_size, state.resize_target(), had_previous_commit,
      was_waiting_for_resize, wm_owns_geometry(state)})) {
  case policy::CommitVerdict::MatchesX11:
    t->reasserted_against.reset(); // the client agrees with us
    t->waiting_for_resize_frame = false;
    cancel_resize_settle(state);
    return false;

  case policy::CommitVerdict::MatchesWmTarget:
    t->reasserted_against.reset(); // the client agrees with us
    // A prior settled, grid-quantized commit may have caused us to shrink the
    // X11 window away from the WM target. If the client later commits a buffer
    // at the requested target size, restore that target instead of treating
    // the frame as stale while leaving the window at the adopted size. This
    // is not adoption of a different geometry: client and WM target agree.
    // For WM-owned geometry, keep the resize prohibition and let the WM's own
    // ConfigureNotify reconcile actual geometry.
    if (wm_owns_geometry(state)) {
      t->waiting_for_resize_frame = true;
      cancel_resize_settle(state);
      return true;
    }
    if (state.x11_window) {
      remember_resize_echo(state, width, height);
      t->x11_size = committed;
      backend.resize_window(state.x11_window, width, height);
    }
    t->waiting_for_resize_frame = false;
    cancel_resize_settle(state);
    return false;

  case policy::CommitVerdict::FirstFrame:
    t->reasserted_against.reset(); // the client agrees with us
    // The first mapped client frame establishes the actual content size.
    // There is no stale frame to stretch yet and no active WM resize to
    // settle. Kept distinct from post-resize adoption.
    adopt_committed_geometry(surface, state, width, height);
    return false;

  case policy::CommitVerdict::Stale:
    break;
  }

  // The client disagrees with a size it cannot get changed by adoption: tell
  // it the size again, once per distinct size it commits.
  if (policy::should_reassert_configure(policy::ReassertInputs{
          committed, t->resize_target(),
          wm_owns_geometry(*t) ||
              (t->wm_target && t->wm_target->overridden),
          state.configure_log.sent.empty(), t->reasserted_against})) {
    t->reasserted_against = committed;
    const policy::WindowSize target = t->resize_target();
    WW_LOG_INFO("[surface] client committed {}x{} but the window manager owns "
                "the size ({}x{}); re-sending the configure",
                width, height, target.width, target.height);
    send_configure(state, target.width, target.height, toplevel_states(*t));
  }

  t->waiting_for_resize_frame = true;
  schedule_resize_settle(surface, state);
  return true;
}

void SurfaceManager::record_configure_serial(wl_resource *surface,
                                             uint32_t serial) {
  auto it = impl_->surfaces.find(surface);
  if (it == impl_->surfaces.end())
    return;
  auto &sent = it->second.configure_log.sent;
  sent.push_back(serial);
  constexpr std::size_t kConfigureHistoryLimit = 4096;
  if (sent.size() > kConfigureHistoryLimit)
    sent.pop_front();
}

bool SurfaceManager::acknowledge_configure(wl_resource *surface,
                                            uint32_t serial) {
  auto it = impl_->surfaces.find(surface);
  if (it == impl_->surfaces.end())
    return false;
  auto &state = it->second;
  const auto acknowledged =
      std::find(state.configure_log.sent.begin(),
                state.configure_log.sent.end(), serial);
  if (acknowledged == state.configure_log.sent.end())
    return false;

  // Acknowledging configure N also discards every older outstanding configure:
  // clients may receive several configures before rendering and are required
  // to acknowledge the one whose state they are about to commit. Keeping old
  // serials valid after that point would incorrectly accept duplicate/stale
  // acknowledgements.
  state.configure_log.sent.erase(state.configure_log.sent.begin(),
                                     std::next(acknowledged));
  state.configure_log.last_acked = serial;
  if (state.waiting_for_resize_frame())
    impl_->schedule_resize_settle(surface, state);
  return true;
}

void SurfaceManager::request_resize(uint32_t x11_window, uint16_t width,
                                    uint16_t height) {
  if (!x11_window || width == 0 || height == 0)
    return;

  auto wit = impl_->surface_index.find_window(x11_window);
  if (wit == impl_->surface_index.window_end()) {
    WW_LOG_WARN("[surface] request_resize: no surface for x11 win {}",
                x11_window);
    return;
  }

  auto sit = impl_->surfaces.find(wit->second);
  if (sit == impl_->surfaces.end())
    return;

  Surface &state = sit->second;
  Toplevel *const t = state.live_toplevel();
  if (!t) {
    WW_LOG_DEBUG(
        "[surface] request_resize ignored: surface {} has no toplevel role yet",
        static_cast<void *>(wit->second));
    return;
  }

  const auto now = impl_->resize_timer ? impl_->resize_timer->now()
                                       : IResizeTimer::Clock::now();
  std::erase_if(t->expected_resize_echoes,
                [now](const ExpectedResizeEcho &echo) {
                  return echo.expires_at <= now;
                });
  const bool is_programmatic_echo = std::any_of(
      t->expected_resize_echoes.begin(), t->expected_resize_echoes.end(),
      [width, height](const ExpectedResizeEcho &echo) {
        return echo.width == width && echo.height == height;
      });
  if (is_programmatic_echo) {
    // Leave x11_width/x11_height at the latest geometry WWindows requested:
    // this event can be a delayed echo for an earlier resize, after a newer
    // resize has already been issued. Keep the entry briefly to suppress
    // duplicates from the same ConfigureNotify burst.
    WW_LOG_DEBUG("[surface] ignored programmatic ConfigureNotify echo (x11={}, "
                 "size={}x{})", x11_window, width, height);
    return;
  }
  // An unmatched event is a new WM target. Earlier echoes were generated
  // before it and can no longer be confused with the current geometry.
  t->expected_resize_echoes.clear();

  // WM target identity is independent of the actual X11 geometry: an adopted
  // client size must not make a duplicate ConfigureNotify for the unchanged
  // WM target look like a new resize generation.
  const policy::WindowSize size{width, height};
  const bool target_changed = !t->wm_target || !t->wm_target->observed ||
                              t->wm_target->size != size;
  t->x11_size = size;
  if (target_changed) {
    WmTarget target;
    target.size = size;
    target.since = now;
    target.generation = t->wm_target ? t->wm_target->generation + 1 : 1;
    target.observed = true;
    t->wm_target = target;
    t->reasserted_against.reset(); // a new target gets a fresh configure
    impl_->cancel_resize_settle(state);
  } else if (t->wm_target->adopted && !t->wm_target->overridden) {
    // Same WM target again, not an echo of our own resize: after we adopted
    // a different (client) size the WM has put the window back, so it is
    // enforcing this size. Leave it there instead of fighting it.
    t->wm_target->overridden = true;
    WW_LOG_INFO("[surface] WM re-asserted its target {}x{} after we adopted "
                "the client size; no longer adopting for this target "
                "(x11={})", width, height, x11_window);
  }

  const bool sent = impl_->configure_if_changed(state, width, height);
  const bool has_matching_client_size =
      state.committed.content_size && *state.committed.content_size == size;
  t->waiting_for_resize_frame = !has_matching_client_size;
  if (t->waiting_for_resize_frame) {
    impl_->schedule_resize_settle(wit->second, state);
  } else {
    impl_->cancel_resize_settle(state);
  }
  // A DMABUF import may still be pending while the WM changes the target.
  // Update its eventual presentation so completion cannot put an unscaled
  // stale frame into a window that is already larger than that frame.
  if (state.presentation.importing) {
    constexpr uint64_t kMaxStretchPixels = 8192ull * 8192ull;
    const bool stretch = t->waiting_for_resize_frame && width != 0 &&
        height != 0 && static_cast<uint64_t>(width) * height <=
                            kMaxStretchPixels;
    state.presentation.importing->stretch_width = stretch ? width : 0;
    state.presentation.importing->stretch_height = stretch ? height : 0;
  }

  if (!sent) {
    WW_LOG_DEBUG("[surface] duplicate ConfigureNotify suppressed: "
                 "{}x{} (x11={})", width, height, x11_window);
    return;
  }

  WW_LOG_INFO("[surface] request_resize (x11={}, surface={}, size={}x{})",
              x11_window, static_cast<void *>(wit->second), width, height);
}

bool SurfaceManager::has_xdg_surface_role(wl_resource *surface_res) const {
  auto it = impl_->surfaces.find(surface_res);
  return it != impl_->surfaces.end() && it->second.has_xdg_surface_role;
}

void SurfaceManager::mark_xdg_surface_role(wl_resource *surface_res) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] mark_xdg_surface_role: unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }
  it->second.has_xdg_surface_role = true;
}

void SurfaceManager::set_window_geometry(wl_resource *surface_res, int32_t x,
                                         int32_t y, int32_t width,
                                         int32_t height) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] set_window_geometry: unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }

  Surface &state = it->second;
  // Double-buffered against this surface's own commit (see
  // PendingState::window_geometry) -- write the pending side
  // only; commit() applies it and, if the effective origin actually moved,
  // repositions children then, not here.
  if (width <= 0 || height <= 0) {
    // Per xdg-shell a zero-sized geometry is meaningless; treat it as
    // "unset" and fall back to the whole buffer rather than blanking the
    // window.
    state.pending.window_geometry = std::optional<WindowGeometry>{};
    WW_LOG_DEBUG("[surface] window geometry cleared, pending next commit "
                "(surface={})",
                static_cast<void *>(surface_res));
    return;
  }

  state.pending.window_geometry = WindowGeometry{x, y, width, height};

  WW_LOG_DEBUG("[surface] window geometry pending next commit (surface={}, "
              "{}x{}+{}+{})",
              static_cast<void *>(surface_res), width, height, x, y);
}

void SurfaceManager::set_wm_state(uint32_t x11_window,
                                  IWindowBackend::WmWindowState state) {
  auto wit = impl_->surface_index.find_window(x11_window);
  if (wit == impl_->surface_index.window_end())
    return;
  auto sit = impl_->surfaces.find(wit->second);
  if (sit == impl_->surfaces.end())
    return;

  Surface &surface_state = sit->second;
  Toplevel *const t = surface_state.as_toplevel();
  if (!t)
    return; // only a toplevel carries WM state
  if (t->wm == state)
    return; // callers must tolerate repeats; nothing to translate

  const bool previously_wm_owned = wm_owns_geometry(*t);
  const IWindowBackend::WmWindowState previous = t->wm;
  t->wm = state;

  // The WM undid a state: whatever the client last asked for is over. Left
  // set it would keep the geometry WM-owned for good, and make the client's
  // next request look like a repeat and be dropped.
  if (previous.fullscreen && !state.fullscreen)
    t->wants_fullscreen = false;
  if ((previous.maximized_horz || previous.maximized_vert) &&
      !state.maximized_horz && !state.maximized_vert)
    t->wants_maximized = false;

  WW_LOG_INFO("[surface] window manager state (x11={}, fullscreen={}, "
              "maximized={}x{})", x11_window, state.fullscreen,
              state.maximized_horz, state.maximized_vert);

  // Fullscreen/maximized (and the tiled edges a both-axes maximize implies)
  // changed what the client should be told: same size, new states.
  if (t->configured)
    impl_->configure_if_changed(surface_state, t->configured->size.width,
                                t->configured->size.height);

  const bool now_wm_owned = wm_owns_geometry(*t);
  if (now_wm_owned) {
    impl_->cancel_resize_settle(surface_state);
  } else if (previously_wm_owned) {
    // Returning to floating mode starts a fresh settle interval: the target
    // may have been stable for a long time while the WM owned it.
    impl_->cancel_resize_settle(surface_state);
    if (t->wm_target && t->wm_target->observed) {
      t->wm_target->restart(impl_->resize_timer
          ? impl_->resize_timer->now() : IResizeTimer::Clock::now());
      if (t->waiting_for_resize_frame)
        impl_->schedule_resize_settle(wit->second, surface_state);
    }
  }
}

void SurfaceManager::Impl::apply_tiled_edges(Surface &state,
                                             uint32_t x11_window,
                                             wl_resource *surface,
                                             TiledEdges edges) {
  Toplevel *const t = state.as_toplevel();
  if (!t || t->tiled == edges)
    return;
  t->tiled = edges;

  WW_LOG_INFO(
      "[surface] toplevel {} by the window manager (x11={}, surface={}, "
      "edges=L{}R{}T{}B{})",
      edges.any() ? "tiled" : "floating", x11_window,
      static_cast<void *>(surface), edges.left, edges.right, edges.top,
      edges.bottom);

  // Same gate as every other configure, at the size the client already has:
  // the pixel dimensions are usually identical before and after (i3 tiles a
  // window into the very rect it was floating in, or vice versa), which is
  // exactly the case the gate's states comparison exists to let through.
  // Before the first configure there is nothing to amend -- the flag is
  // simply picked up by that one.
  if (t->configured)
    configure_if_changed(state, t->configured->size.width,
                         t->configured->size.height);
  schedule_resize_settle(surface, state);
}

void SurfaceManager::set_tiled(uint32_t x11_window, bool tiled) {
  if (!x11_window)
    return;

  auto wit = impl_->surface_index.find_window(x11_window);
  if (wit == impl_->surface_index.window_end())
    return; // some other client's window; the watcher reports them all

  auto sit = impl_->surfaces.find(wit->second);
  if (sit == impl_->surfaces.end())
    return;

  impl_->apply_tiled_edges(sit->second, x11_window, wit->second,
                           TiledEdges{tiled, tiled, tiled, tiled});
}

void SurfaceManager::set_tiled_edges(uint32_t x11_window, TiledEdges edges) {
  if (!x11_window)
    return;

  auto wit = impl_->surface_index.find_window(x11_window);
  if (wit == impl_->surface_index.window_end())
    return; // some other client's window; the watcher reports them all

  auto sit = impl_->surfaces.find(wit->second);
  if (sit == impl_->surfaces.end())
    return;

  impl_->apply_tiled_edges(sit->second, x11_window, wit->second, edges);
}

void SurfaceManager::set_activated_surface(wl_resource *focused_res) {
  // Which surface holds keyboard focus is decided by X (via the input
  // translator); this only mirrors it into each toplevel's ACTIVATED state.
  // Nothing is remembered besides the per-toplevel flag, so a surface that
  // was destroyed while active leaves nothing dangling.
  //
  // Focus can sit on a popup (a menu with a grab) or a subsurface; the
  // window the user is "in" is still the toplevel at the root of its parent
  // chain, and that toplevel must stay activated -- a client that is told it
  // was deactivated when its own menu opened may well dismiss the menu.
  wl_resource *surface_res = focused_res;
  for (int depth = 0; surface_res && depth < 64; ++depth) {
    auto it = impl_->surfaces.find(surface_res);
    if (it == impl_->surfaces.end() || it->second.as_toplevel())
      break;
    wl_resource *parent = it->second.parent_surface();
    if (!parent)
      break; // a role-less or cursor surface: nothing to activate above it
    surface_res = parent;
  }
  for (auto &[res, state] : impl_->surfaces) {
    Toplevel *const t = state.as_toplevel();
    if (!t)
      continue;
    const bool should_be_active = surface_res && res == surface_res;
    if (t->activated == should_be_active)
      continue;
    t->activated = should_be_active;
    // Same size, new states. Before the first configure there is nothing to
    // re-send: the flag simply rides along on it.
    if (t->configured)
      impl_->configure_if_changed(state, t->configured->size.width,
                                  t->configured->size.height);
  }
}

void SurfaceManager::set_toplevel_state(wl_resource *surface_res,
                                        WindowState st, bool enabled) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end())
    return;

  Surface &state = it->second;
  Toplevel *const t = state.as_toplevel();
  if (!state.x11_window || !t || !t->toplevel)
    return;

  switch (st) {
  case WindowState::Fullscreen:
    if (t->wants_fullscreen == enabled)
      return;
    t->wants_fullscreen = enabled;
    break;
  case WindowState::Maximized:
    if (t->wants_maximized == enabled)
      return;
    t->wants_maximized = enabled;
    break;
  }

  WW_LOG_INFO("[surface] toplevel state {} {} (x11={}, surface={})",
              st == WindowState::Fullscreen ? "fullscreen" : "maximized",
              enabled ? "requested" : "dropped", state.x11_window,
              static_cast<void *>(surface_res));

  impl_->backend.set_window_state(state.x11_window, st, enabled);

  // xdg-shell has the compositor answer set_fullscreen/set_maximized (and the
  // unset forms) with a configure whether or not it complies, so the client
  // is not left waiting on a request a window manager may have refused. The
  // states in it are what the WM has actually done so far -- not yet this
  // request: if the WM complies, observing that (set_wm_state) sends the
  // configure that carries the new state and the new geometry.
  if (t->configured && state.live_toplevel())
    send_configure(state, t->configured->size.width,
                   t->configured->size.height, toplevel_states(*t));
  impl_->schedule_resize_settle(surface_res, state);
}

void SurfaceManager::move_toplevel(wl_resource *surface_res) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end())
    return;
  Surface &state = it->second;
  if (!state.x11_window || !state.has_toplevel())
    return;

  WW_LOG_DEBUG("[surface] interactive move requested (x11={}, surface={})",
              state.x11_window, static_cast<void *>(surface_res));
  impl_->backend.request_interactive_move(state.x11_window);
}

void SurfaceManager::resize_toplevel(wl_resource *surface_res,
                                     uint32_t edges) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end())
    return;
  Surface &state = it->second;
  if (!state.x11_window || !state.has_toplevel())
    return;

  // xdg_toplevel_resize_edge's wire values match ResizeEdge's numerically
  // for every value the protocol itself defines (see window_backend.hpp).
  // A client sending something outside that set is a protocol violation
  // this cast alone wouldn't catch, so validate against the known values
  // explicitly rather than reinterpret an arbitrary bit pattern as a real
  // edge; None is the same "no edge specified" fallback a backend uses
  // for a legitimately-None request.
  ResizeEdge edge = ResizeEdge::None;
  switch (edges) {
  case static_cast<uint32_t>(ResizeEdge::None):
  case static_cast<uint32_t>(ResizeEdge::Top):
  case static_cast<uint32_t>(ResizeEdge::Bottom):
  case static_cast<uint32_t>(ResizeEdge::Left):
  case static_cast<uint32_t>(ResizeEdge::TopLeft):
  case static_cast<uint32_t>(ResizeEdge::BottomLeft):
  case static_cast<uint32_t>(ResizeEdge::Right):
  case static_cast<uint32_t>(ResizeEdge::TopRight):
  case static_cast<uint32_t>(ResizeEdge::BottomRight):
    edge = static_cast<ResizeEdge>(edges);
    break;
  default:
    WW_LOG_WARN("[surface] resize_toplevel: unrecognized edge {} — "
                "treating as None (x11={})",
                edges, state.x11_window);
    break;
  }

  WW_LOG_DEBUG("[surface] interactive resize requested (edges={}, x11={}, "
              "surface={})",
              edges, state.x11_window, static_cast<void *>(surface_res));
  impl_->backend.request_interactive_resize(state.x11_window, edge);
}

void SurfaceManager::minimize_toplevel(wl_resource *surface_res) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end())
    return;
  Surface &state = it->second;
  if (!state.x11_window || !state.has_toplevel())
    return;

  WW_LOG_DEBUG("[surface] minimize requested (x11={}, surface={})",
              state.x11_window, static_cast<void *>(surface_res));
  impl_->backend.minimize_window(state.x11_window);
}

void SurfaceManager::set_toplevel(wl_resource *surface_res,
                                  wl_resource *toplevel_res,
                                  wl_resource *xdg_surface_res) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] set_toplevel: unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }

  Surface &surf = it->second;
  if (surf.has_role() && !surf.as_toplevel()) {
    WW_LOG_WARN("[surface] set_toplevel: surface {} already has a different "
                "role", static_cast<void *>(surface_res));
    return;
  }
  // A role can be re-established on an existing surface in tests and during
  // unusual client teardown sequences; the existing Toplevel state is kept
  // and only its resources are replaced. Do not let a timer from its previous
  // configure lifetime race the new toplevel state.
  if (!surf.as_toplevel())
    surf.role = Toplevel{};
  Toplevel &top = *surf.as_toplevel();
  impl_->cancel_resize_settle(surf);

  if (!it->second.x11_window) {
    const uint32_t win = impl_->backend.create_window(800, 600);
    // Deliberately not mapped yet -- see Surface::x11_map_requested. The
    // window is created and identified now, described by the requests that
    // follow (set_title, set_app_id, ...), and mapped by commit().
    it->second.x11_window = win;
    top.x11_size = policy::WindowSize{800, 600};

    // Who owns it is known from the socket, not from anything the client
    // says, so it is settled here and not left for a later request.
    pid_t pid = 0;
    if (wl_client *client = wl_resource_get_client(surface_res))
      wl_client_get_credentials(client, &pid, nullptr, nullptr);
    if (pid > 0)
      impl_->backend.set_window_client_process(win,
                                               static_cast<uint32_t>(pid));

    impl_->surface_index.bind_window(win, surface_res);
    WW_LOG_INFO("[surface] {} promoted to toplevel -> x11 win {}",
                static_cast<void *>(surface_res), win);
  }

  if (top.toplevel)
    wl_list_remove(&it->second.toplevel_destroy_listener.link);
  top.toplevel = toplevel_res;
  it->second.toplevel_destroy_listener.notify = Impl::on_toplevel_destroyed;
  wl_resource_add_destroy_listener(toplevel_res,
                                   &it->second.toplevel_destroy_listener);

  if (top.xdg_surface)
    wl_list_remove(&it->second.xdg_surface_destroy_listener.link);
  top.xdg_surface = xdg_surface_res;
  it->second.xdg_surface_destroy_listener.notify = Impl::on_xdg_surface_destroyed;
  wl_resource_add_destroy_listener(xdg_surface_res,
                                   &it->second.xdg_surface_destroy_listener);

  WW_LOG_DEBUG(
      "[surface] set_toplevel (surface={}, xdg_surface={}, toplevel={}, x11={})",
      static_cast<void *>(surface_res), static_cast<void *>(xdg_surface_res),
      static_cast<void *>(toplevel_res), it->second.x11_window);

  // Seeds Toplevel::configured as well, so the window manager's first
  // ConfigureNotify is compared against something real rather than being
  // treated as the first configure this surface has ever seen.
  send_configure(it->second, 800, 600, toplevel_states(top));
  WmTarget seed; // provisional: observed == false
  seed.size = policy::WindowSize{800, 600};
  seed.since = impl_->resize_timer ? impl_->resize_timer->now()
                                   : IResizeTimer::Clock::now();
  seed.generation = top.wm_target ? top.wm_target->generation + 1 : 1;
  top.wm_target = seed;
}

void SurfaceManager::set_title(wl_resource *surface_res, const char *title) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] set_title: unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }

  if (it->second.x11_window) {
    impl_->backend.set_window_title(it->second.x11_window, title);
    WW_LOG_DEBUG("[surface] set title (surface={}, x11={}, title=\"{}\")",
                 static_cast<void *>(surface_res), it->second.x11_window,
                 title ? title : "");
  }
}

void SurfaceManager::set_app_id(wl_resource *surface_res,
                                const char *app_id) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] set_app_id: unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }
  if (!it->second.x11_window)
    return;

  // A Wayland app_id is one string (conventionally the desktop-file id, or
  // for older clients the program name); X11's WM_CLASS is an
  // (instance, class) pair. Both halves carry the app_id verbatim so a
  // window-manager rule written against either one matches.
  impl_->backend.set_window_class(it->second.x11_window, app_id, app_id);
  WW_LOG_DEBUG("[surface] set app_id (surface={}, x11={}, app_id=\"{}\")",
               static_cast<void *>(surface_res), it->second.x11_window,
               app_id ? app_id : "");
}

void SurfaceManager::set_toplevel_parent(wl_resource *surface_res,
                                         wl_resource *parent_surface_res) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end()) {
    WW_LOG_WARN("[surface] set_parent: unknown surface {}",
                static_cast<void *>(surface_res));
    return;
  }
  if (!it->second.x11_window)
    return;

  uint32_t parent_window = 0;
  if (parent_surface_res && parent_surface_res != surface_res) {
    if (auto pit = impl_->surfaces.find(parent_surface_res);
        pit != impl_->surfaces.end())
      parent_window = pit->second.x11_window;
  }

  impl_->backend.set_window_transient_for(it->second.x11_window,
                                          parent_window);
  WW_LOG_DEBUG("[surface] set parent (surface={}, x11={}, parent x11={})",
               static_cast<void *>(surface_res), it->second.x11_window,
               parent_window);
}

void SurfaceManager::set_min_size(wl_resource *surface_res, int32_t width,
                                  int32_t height) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end())
    return;
  // Negative is a protocol error in the client; be lenient and read it as
  // "no limit" rather than hand the window system a huge unsigned number.
  auto &limits = pending_size_limits(it->second);
  limits.min_width = std::max(width, 0);
  limits.min_height = std::max(height, 0);
}

void SurfaceManager::set_max_size(wl_resource *surface_res, int32_t width,
                                  int32_t height) {
  auto it = impl_->surfaces.find(surface_res);
  if (it == impl_->surfaces.end())
    return;
  auto &limits = pending_size_limits(it->second);
  limits.max_width = std::max(width, 0);
  limits.max_height = std::max(height, 0);
}

void SurfaceManager::request_close(uint32_t x11_window) {
  if (!x11_window)
    return;

  auto wit = impl_->surface_index.find_window(x11_window);
  if (wit == impl_->surface_index.window_end()) {
    WW_LOG_WARN("[surface] request_close: no surface for x11 win {}",
                x11_window);
    return;
  }

  auto sit = impl_->surfaces.find(wit->second);
  if (sit == impl_->surfaces.end())
    return;

  const Toplevel *const top = sit->second.as_toplevel();
  wl_resource *toplevel = top ? top->toplevel : nullptr;
  if (!toplevel) {
    WW_LOG_DEBUG("[surface] request_close ignored: surface {} has no toplevel",
                 static_cast<void *>(wit->second));
    return;
  }

  WW_LOG_INFO("[surface] request_close (x11={}, surface={})", x11_window,
              static_cast<void *>(wit->second));

  xdg_toplevel_send_close(toplevel);
}

} // namespace ww
