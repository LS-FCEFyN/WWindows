#pragma once
// X11Backend::Impl -- the composition root for every piece of X11-side state.
//
// The state used to be one ~200-field struct. It is now five subsystem
// structs, each in its own header beside this one and each a pure data
// holder:
//
//   ClipboardState      detail/clipboard.hpp   CLIPBOARD selection, both ICCCM roles
//   PresentState        detail/present.hpp     DRI3 / Present / Render, retained frames
//   ShmBackendState     detail/shm.hpp         MIT-SHM present segments
//   WindowBackendState  detail/window.hpp      atoms, GCs, depths, transient-for
//   InputBackendState   detail/input.hpp       XKB state, input callbacks
//
// Behaviour did not move: every state machine is still a static method on
// Impl, because most of them reach across subsystems (dispatch_one routes
// SHM completions, XKB notifies, Present events and clipboard
// PropertyNotify in one pass). Only the field access path changed, from
// `im.field` to `im.<subsystem>.field`.
//
// What stays directly on Impl is what is genuinely shared: the connection,
// the screen, the event loop, the callbacks the backend hands out, and a few
// one-off flags/handles no single subsystem owns.

#include "wwindows/adapters/x11/x11_backend.hpp"
#include "wwindows/adapters/x11/detail/clipboard.hpp"
#include "wwindows/adapters/x11/detail/input.hpp"
#include "wwindows/adapters/x11/detail/present.hpp"
#include "wwindows/adapters/x11/detail/shm.hpp"
#include "wwindows/adapters/x11/detail/window.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include <xcb/xcb.h>

namespace ww {

struct X11Backend::Impl {
  // --- Shared across subsystems -------------------------------------------
  xcb_connection_t *conn{nullptr};
  xcb_screen_t *screen{nullptr};
  EventLoop *loop{nullptr}; // stashed at create() to (de)register pipe fds
  ResizeCallback on_resize;
  WmStateCallback on_wm_state;
  std::function<void(uint32_t)> close_cb;
  std::function<void()> events_processed_cb;
  // See watch_root_property()'s doc comment. Keyed by the interned atom
  // for the property name the caller asked about.
  std::unordered_map<xcb_atom_t, std::function<void(std::optional<std::string>)>> root_property_watches;

  bool has_shape{false};
  // Lazily created transparent cursor behind IWindowBackend::blank_cursor().
  xcb_cursor_t blank_cursor_id{XCB_CURSOR_NONE};
  uint64_t diag_timer{0};

  // --- Subsystems ----------------------------------------------------------
  ClipboardState clipboard;
  PresentState present;
  ShmBackendState shm;
  WindowBackendState window;
  InputBackendState input;

  static void intern_clipboard_atoms(Impl &im);
  static void refresh_incoming_clipboard_targets(Impl &im);
  static void start_next_clipboard_receive(Impl &im);
  static void finish_clipboard_send(Impl &im, int pipe_rfd, bool success);
  static bool start_clipboard_send(Impl &im, xcb_window_t requestor,
                                   xcb_atom_t property, xcb_atom_t target,
                                   uint64_t multiple_id = 0,
                                   std::size_t pair_index = 0);
  static void finish_clipboard_multiple(Impl &im, uint64_t multiple_id);
  static void poll_pending_clipboard_multiple_reads(Impl &im);
  static void start_incr_receive_chunk_read(Impl &im);
  // Shared tail of both the immediate (property==NONE) and the
  // deferred-via-poll_pending_property_read paths through
  // dispatch_selection_notify: interpret `data` according to what was
  // requested and hand it to the right callback, then start the next
  // queued receive.
  static void finish_clipboard_receive(Impl &im, PendingClipboardReceive req,
                                       std::vector<uint8_t> data);
  static void finish_clipboard_targets(Impl &im,
                                       PendingClipboardTargets pending);
  static void poll_pending_clipboard_targets(Impl &im);

  static void dispatch_clipboard_property_notify(Impl &im,
                                                 xcb_property_notify_event_t *event);
  static void dispatch_selection_request(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_selection_clear(Impl &im, xcb_generic_event_t *ev);
  // INCR transfers whose peer vanished or stalled (see kClipboardIncrIdleTimeout).
  static void dispatch_destroy_notify(Impl &im, xcb_generic_event_t *ev);
  static void expire_stale_incr_transfers(Impl &im);
  // Abandon the in-progress INCR *receive* and complete the pending paste with
  // empty data so the Wayland client's pipe reaches EOF instead of hanging.
  static void abort_incr_receive(Impl &im, const char *why);
  static void dispatch_selection_notify(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_xfixes_selection_notify(Impl &im,
                                               xcb_generic_event_t *ev);
  // Non-blocking check for pending_property_read's reply, called from
  // process_events() on every pass through the event loop. A no-op when
  // nothing is pending, and does nothing (leaves it pending) until the
  // reply is actually ready — see PendingPropertyRead's comment.
  static void poll_pending_property_read(Impl &im);
  // Same idea as poll_pending_property_read, for pending_dmabuf_imports —
  // see PendingDmabufImport's comment. Also called every pass through
  // process_events().
  static void poll_pending_dmabuf_imports(Impl &im);
  // A managed toplevel's _NET_WM_STATE changed: start a non-blocking read,
  // then interpret the reply. See PendingWmStateRead.
  static void dispatch_property_notify(Impl &im, xcb_generic_event_t *ev);
  // Shared by root_string_property() (which interns `name` first) and
  // dispatch_property_notify() (which already has the atom off the event,
  // and a delete notification it handles separately without this at all).
  // format!=8 or an absent/empty value both come back as std::nullopt --
  // callers that need to tell "unreadable" apart from "empty string" don't
  // exist yet.
  static std::optional<std::string> read_root_string_property(Impl &im,
                                                               xcb_atom_t atom);
  static void poll_pending_wm_state_reads(Impl &im);
  static void poll_pending_frame_fences(Impl &im);
  static void retire_retained_pixmap(Impl &im, uint32_t window,
                                     uint32_t pixmap);
  static void free_pixmap_resource(Impl &im, uint32_t pixmap);
  static void release_retired_pixmaps_for_window(Impl &im, uint32_t window);
  // Shared tail of both outcomes a pending DRI3 import can resolve to
  // (dispatch_one's error-match, or poll_pending_dmabuf_imports' fence
  // completion): on success, sets up the Render source Picture if
  // pending.source_pictformat is available, then invokes pending.callback with
  // the pixmap id (0 on failure, whether from dispatch_one's error match
  // or from Render setup itself failing). Implemented in
  // X11Backend_dri3.cpp, alongside the rest of the import logic.
  static void finish_dmabuf_import(Impl &im, PendingDmabufImport pending,
                                   bool ok);

  // Drains every pending XCB event on `conn` and dispatches it to the
  // registered resize/input/close callbacks. Registered as the EventLoop
  // readiness callback in X11Backend_lifecycle.cpp; implemented in
  // X11Backend_events.cpp.
  static void process_events(Impl &im);
  // Dispatch events that xcb already queued but poll() cannot see. Returns
  // how many were found (diagnostic + workaround).
  static int drain_stranded(Impl &im);
  static void arm_diag_tick(EventLoop &loop, Impl *im);

  // One-time connection setup, implemented in X11Backend_lifecycle.cpp.
  static void intern_wm_atoms(Impl &im);
  static void find_argb_visual(Impl &im);

  // Render extension: composites a BGR-order or scaled dmabuf source Picture
  // (see RenderSource) onto a window's destination Picture (window_pictures).
  // Implemented in X11Backend_render.cpp, alongside init_render() (the
  // one-time PictFormat query these both depend on).
  static void present_pixmap_via_render(Impl &im, xcb_window_t window,
                                        const RenderSource &src,
                                        BufferRegion crop, int32_t scale,
                                        uint16_t stretch_width = 0,
                                        uint16_t stretch_height = 0);
  // Copies an imported dmabuf pixmap into a compositor-owned X11 pixmap (and a
  // matching Render source Picture when the original has one). Returns 0 when
  // the source is unknown or the copy cannot be issued.
  static uint32_t snapshot_pixmap(Impl &im, uint32_t source);

  // Shared by create_window() (parent = screen->root) and
  // create_child_window() (parent = another client window, for
  // wl_subsurface). Handles visual/depth selection, Present registration,
  // and — only when parenting to root — WM_PROTOCOLS/WM_DELETE_WINDOW.
  // Implemented in X11Backend_window.cpp.
  static uint32_t create_window_impl(Impl &im, xcb_window_t parent,
                                     int16_t x, int16_t y, uint16_t width,
                                     uint16_t height);

  // Shared by request_interactive_move() and request_interactive_resize()
  // (WindowState::* has no equivalent since set_window_state's two states
  // don't need a pointer position). Queries the live pointer position and
  // held button, then sends _NET_WM_MOVERESIZE with the given direction
  // (one of policy::net_wm_moveresize_direction()'s return values, or
  // policy::net_wm_moveresize_move_direction for a move). Implemented in
  // X11Backend_window.cpp.
  static void send_net_wm_moveresize(Impl &im, uint32_t window,
                                     uint32_t direction);

  // Event dispatch — implemented in X11Backend_events.cpp. dispatch_one()
  // is the sole entry point (called from process_events()); the rest are
  // private helpers, one per XCB event type it delegates to.
  static void dispatch_one(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_client_message(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_configure_notify(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_enter_notify(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_leave_notify(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_motion_notify(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_button_press(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_button_release(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_key_press(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_key_release(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_expose(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_present_generic(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_focus_in(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_focus_out(Impl &im, xcb_generic_event_t *ev);
  // XKB extension events share one base event number for every sub-type
  // (StateNotify, MapNotify, ...); the actual sub-type lives inside the
  // event's own xkbType field, checked in the implementation. Only
  // StateNotify is acted on.
  static void dispatch_xkb_event(Impl &im, xcb_generic_event_t *ev);
  static void dispatch_randr_screen_change(Impl &im, xcb_generic_event_t *ev);

  // The screen as the server last reported it, and the RandR plumbing that
  // keeps it current. See X11Backend::screen_geometry().
  struct ScreenState {
    X11Backend::ScreenGeometry geometry;
    bool has_randr{false};
    uint8_t randr_first_event{0};
    std::function<void(const X11Backend::ScreenGeometry &)> on_changed;
  } screen_state;
  // The current refresh rate in mHz via RandR's screen info; 0 if unknown.
  // One blocking round trip -- startup and RandR notifications only.
  static uint32_t query_refresh_mhz(Impl &im);
  static void deliver_xkb_state_notify(Impl &im,
                                       const PendingXkbStateNotify &state);
  static void flush_pending_xkb_state_notifies(Impl &im);
};

} // namespace ww
