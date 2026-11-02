#pragma once
// Window bookkeeping and ICCCM/EWMH atoms owned by X11Backend::Impl.
//
// Visual/colormap for 32-bit windows, the WM_PROTOCOLS / _NET_WM_* atoms
// cached at setup time, per-window GCs and depths, transient-for tracking and
// the in-flight _NET_WM_STATE reads. Driven by X11Backend_window.cpp,
// X11Backend_lifecycle.cpp and X11Backend_events.cpp.
//
// Pure data: no methods.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xcb/xcb.h>

namespace ww {

// One outstanding read of a toplevel's _NET_WM_STATE, fired from a
// PropertyNotify and collected from poll_pending_wm_state_reads(). Never
// blocked on, for the same reason as PendingPropertyRead: this starts from
// inside event dispatch, exactly when a window manager may be holding a
// server grab. Several may be in flight for one window; their replies arrive
// in request order, so the last one applied is the newest.
struct PendingWmStateRead {
  unsigned int sequence{0};
  uint32_t window{0};
};

struct WindowBackendState {
  // Windows created with the root as parent, i.e. the ones a window manager
  // manages. Only these carry a meaningful _NET_WM_STATE; a subsurface's
  // child window is never managed.
  std::unordered_set<uint32_t> toplevels;
  std::vector<PendingWmStateRead> pending_wm_state_reads;

  xcb_visualid_t visual32{0};
  xcb_colormap_t colormap32{0};
  xcb_atom_t wm_protocols_atom{0};
  xcb_atom_t wm_delete_atom{0};
  // Cached at setup time (intern_wm_atoms) so set_window_title() never
  // has to make a blocking XCB round-trip while the event loop is live --
  // see set_window_title()'s comment for why that matters.
  xcb_atom_t net_wm_name_atom{0};
  // _NET_WM_PID: the only property set_window_client_process() writes that
  // is not a predefined atom (WM_CLASS, WM_NORMAL_HINTS, WM_TRANSIENT_FOR
  // and WM_CLIENT_MACHINE all have fixed ids in xcb_atom_t).
  xcb_atom_t net_wm_pid_atom{0};
  // This machine's host name, read once at startup for WM_CLIENT_MACHINE.
  std::string client_machine;
  // EWMH window states, cached for set_window_state(). Zero when the atom
  // couldn't be interned, in which case that state is simply never asked
  // for (a client's set_fullscreen becomes a no-op rather than an error).
  xcb_atom_t net_wm_state_atom{0};
  xcb_atom_t net_wm_state_fullscreen_atom{0};
  xcb_atom_t net_wm_state_maximized_vert_atom{0};
  xcb_atom_t net_wm_state_maximized_horz_atom{0};
  // _NET_WM_MOVERESIZE (request_interactive_move/resize) and
  // WM_CHANGE_STATE (minimize_window) -- see X11Backend_window.cpp. Zero
  // when the atom couldn't be interned, in which case that request is
  // simply never sent (a no-op, same as the net_wm_state_* atoms above).
  xcb_atom_t net_wm_moveresize_atom{0};
  xcb_atom_t wm_change_state_atom{0};

  // Child X11 window -> the X11 window it is currently transient for.
  // Absent (or 0) means "no transient parent".
  //
  // Why this exists although WM_TRANSIENT_FOR is already an X property: X11
  // can only answer "what is window C transient for?", never the reverse
  // "which windows are transient for W?", and destroy_window() needs the
  // reverse (ICCCM 4.1.2.6: children of a destroyed owner are re-parented to
  // its own owner). Answering it from X would mean reading the property of
  // every window we own, synchronously, on the destroy path. So this is a
  // reverse index, not a second source of truth.
  //
  // Synchronization policy: WWindows is the only writer of WM_TRANSIENT_FOR
  // on these windows (it created them, and the property is only ever set via
  // set_window_transient_for()), so every change is mirrored here at the
  // moment it is made and no external change can arrive. If that ever stops
  // being true, the fix is to update this map from the PropertyNotify for
  // WM_TRANSIENT_FOR, not to query X on destroy. Entries are erased when the
  // child or its parent is destroyed (see destroy_window()).
  std::unordered_map<uint32_t, uint32_t> transient_parents;

  // One GC per window -- created on first present, freed on destroy_window.
  // Avoids xcb_create_gc / xcb_free_gc on every single frame.
  std::unordered_map<uint32_t, xcb_gcontext_t> gcs;

  // Depth (bits per pixel) at which each window was created.
  // xcb_shm_put_image requires the image depth to match the drawable depth;
  // passing root_depth (24) for a 32-bit window causes XCB_MATCH (error 8).
  std::unordered_map<uint32_t, uint8_t> window_depths;
};

} // namespace ww
