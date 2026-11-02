#include "wwindows/core/detail/surface_manager_impl.hpp"

#include <wwindows/support/log.hpp>
#include <wayland-server-core.h>

namespace ww {

void SurfaceManager::Impl::on_resource_destroyed(wl_listener *listener, void *) {
  Surface *state = wl_container_of(listener, state, destroy_listener);
  wl_list_remove(&listener->link);
  wl_list_init(&listener->link);
  WW_LOG_TRACE("[surface] pending buffer destroyed out from under us");
  if (state->presentation.importing &&
      state->presentation.importing->buffer ==
          state->pending.attached_buffer()) {
    // Release any frame callbacks held by an import that can no longer be
    // used. The backend callback may still arrive, but its generation/state
    // checks will prevent it from presenting this resource.
    state->presentation.cancel_import();
  }
  // Resource destruction is not wl_surface.attach(nullptr). Keep the
  // current committed image unless the client explicitly detached it.
  state->pending.attach = PendingAttachInvalidated{};
}

void SurfaceManager::Impl::on_last_buffer_destroyed(wl_listener *listener, void *) {
  Surface *state = wl_container_of(listener, state, last_buffer_destroy_listener);
  wl_list_remove(&listener->link);
  wl_list_init(&listener->link);
  WW_LOG_TRACE("[surface] last buffer destroyed out from under us");
  if (state->presentation.importing &&
      state->presentation.importing->buffer == state->last_buffer) {
    state->presentation.cancel_import();
  }
  state->last_buffer = nullptr;
}

void SurfaceManager::Impl::detach_current_buffer(Surface &state) {
  if (state.last_buffer) {
    wl_list_remove(&state.last_buffer_destroy_listener.link);
    wl_list_init(&state.last_buffer_destroy_listener.link);
  }
  state.last_buffer = nullptr;
  state.committed.content_size.reset();
  if (auto *t = state.as_toplevel())
    t->waiting_for_resize_frame = false;
  cancel_resize_settle(state);
  state.content.clear();
  state.presentation.cancel_import();
  if (state.x11_window) {
    backend.clear_retained_frame(state.x11_window);
    if (state.x11_map_requested)
      backend.unmap_window(state.x11_window);
  }
  state.x11_map_requested = false;
}

void SurfaceManager::Impl::on_toplevel_destroyed(wl_listener *listener, void *) {
  Surface *state = wl_container_of(listener, state, toplevel_destroy_listener);
  wl_list_remove(&listener->link);
  WW_LOG_DEBUG("[surface] toplevel resource destroyed");
  if (auto *t = state->as_toplevel())
    t->toplevel = nullptr;
}

void SurfaceManager::Impl::on_xdg_surface_destroyed(wl_listener *listener, void *) {
  Surface *state = wl_container_of(listener, state, xdg_surface_destroy_listener);
  wl_list_remove(&listener->link);
  WW_LOG_DEBUG("[surface] xdg_surface resource destroyed");
  if (auto *t = state->as_toplevel())
    t->xdg_surface = nullptr;
}

} // namespace ww
