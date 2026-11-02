#pragma once
// MIT-SHM presentation state owned by X11Backend::Impl.
//
// Two shared-memory segments per window, reused and grown across
// present_pixels() calls. A segment is never overwritten until its
// XCB_SHM_COMPLETION event arrives. Implemented by X11Backend_present.cpp.
//
// Pure data (plus the one free function that releases a segment).

#include "wwindows/ports/window_backend.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include <xcb/shm.h>
#include <xcb/xcb.h>

namespace ww {

// A single shared-memory segment, reused (and grown) across present_pixels()
// calls to the window/slot it belongs to, avoiding a shmget/shmat/shmctl
// round trip on every frame.
struct ShmState {
  xcb_shm_seg_t seg{0};
  int shmid{-1};
  void *data{nullptr};
  size_t size{0};
  bool in_flight{false}; // server has not yet sent XCB_SHM_COMPLETION
  // Sequence number of the PutImage that set in_flight. A failed PutImage is
  // reported only as an async X error and never produces XCB_SHM_COMPLETION,
  // so the error handler matches on this to release the segment and fire
  // on_presented (otherwise the client's wl_surface.frame never completes).
  uint32_t put_sequence{0};
  IWindowBackend::FramePresentedCallback on_presented;
};

// Detaches and frees a single segment (server-side xcb_shm_detach, then our
// own shmdt/shmctl), leaving it default-constructed. Safe to call on an
// already-empty ShmState (shmid == -1) -- becomes a no-op. Shared by the
// growth path in present_pixels() and the teardown paths in
// destroy_window()/forget_window()/~X11Backend() so the free sequence is
// written once.
void free_shm_state(xcb_connection_t *conn, ShmState &shm);

// If both SHM segments are in flight, retain only the newest frame. This
// bounds memory while ensuring a temporarily busy X server cannot cause us
// to overwrite a segment it is still reading.
struct PendingShmFrame {
  bool present{false};
  uint16_t width{0};
  uint16_t height{0};
  uint32_t stride{0};
  BufferRegion src{};
  std::vector<uint8_t> pixels;
  IWindowBackend::FramePresentedCallback on_presented;
};

// Two segments per window. A segment is reusable only after the server's
// XCB_SHM_COMPLETION event, not merely because two other frames were queued.
struct WindowShm {
  ShmState buffers[2];
  int next{0};
  PendingShmFrame pending;
};

struct ShmBackendState {
  bool has_shm{false};
  uint8_t shm_first_event{0};
  uint8_t shm_major_opcode{0};

  // present_pixels() SHM segments, two per window, created lazily and grown
  // as needed. A segment is never overwritten until its XCB_SHM_COMPLETION
  // event arrives. If both segments are in flight, WindowShm retains only the
  // newest pending frame and retries it on completion; memory use stays
  // bounded while a slow X server cannot cause a torn frame.
  std::unordered_map<uint32_t, WindowShm> window_shm;
  std::function<void(uint32_t, PendingShmFrame)> present_pending_shm_frame;
};

} // namespace ww
