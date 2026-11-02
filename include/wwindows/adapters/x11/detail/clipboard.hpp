#pragma once
// Clipboard (CLIPBOARD selection) state owned by X11Backend::Impl.
//
// WWindows plays both ICCCM roles: owner (a Wayland client called
// wl_data_device.set_selection) and requestor (a native X11 application owns
// CLIPBOARD and a Wayland client wants to paste). The state for both roles
// lives in ClipboardState; the request/response state machines that drive it
// are the static clipboard methods on X11Backend::Impl
// (X11Backend_clipboard.cpp).
//
// Pure data: no methods. Split out of x11_backend_impl.hpp so that header is
// a readable composition root rather than a ~200-field list.

#include "wwindows/ports/window_backend.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <xcb/xcb.h>

namespace ww {

// --- Clipboard (CLIPBOARD selection) internals --------------------------
//
// WWindows is both a potential *owner* of CLIPBOARD (when a Wayland client
// sets the Wayland selection — native X11 apps then paste from it) and a
// *requestor* (when some X11 application owns CLIPBOARD and a Wayland
// client wants to paste from it). MIME-named atoms carry arbitrary payloads;
// ICCCM INCR chunks payloads larger than a single X11 property. Total data
// is still bounded to prevent untrusted clipboard owners from exhausting RAM.
inline constexpr uint32_t kClipboardMaxBytes = 64u << 20; // 64 MiB
inline constexpr uint32_t kClipboardIncrChunkBytes = 64u << 10; // 64 KiB
// An INCR transfer (either direction) that sees no progress for this long is
// abandoned. Covers a peer that is alive but stuck; a peer that dies is
// handled immediately (DestroyNotify / XFixes owner change).
inline constexpr std::chrono::seconds kClipboardIncrIdleTimeout{30};

// One SelectionRequest we're actively answering (we are the owner): the
// requested mime type's bytes are being read from `pipe_rfd` (the write
// end was handed to the owning wl_data_source via ClipboardSendCallback)
// and accumulated here until EOF, then written into the requestor's
// property and a SelectionNotify is sent.
struct PendingClipboardSend {
  xcb_window_t requestor{0};
  xcb_atom_t property{0};
  xcb_atom_t target{0};
  std::vector<uint8_t> data;
  uint64_t multiple_id{0};
  std::size_t multiple_pair_index{0};
};

struct PendingClipboardIncrSend {
  xcb_window_t requestor{0};
  xcb_atom_t property{0};
  xcb_atom_t target{0};
  std::vector<uint8_t> data;
  std::size_t offset{0};
  std::chrono::steady_clock::time_point last_activity{
      std::chrono::steady_clock::now()};
};

// One outstanding request *we* issued (ConvertSelection), waiting for the
// SelectionNotify reply on our selection_window. `is_targets_query` is set
// when this is the "what mime types does the current owner support" probe
// (see refresh_incoming_clipboard_targets) rather than an actual data
// fetch requested by a Wayland client.
struct PendingClipboardReceive {
  bool is_targets_query{false};
  std::string mime_type; // empty for a targets query
  std::function<void(std::vector<uint8_t>)> on_data;
};

struct PendingClipboardIncrReceive {
  PendingClipboardReceive req;
  xcb_atom_t property{0};
  std::vector<uint8_t> data;
  std::chrono::steady_clock::time_point last_activity{
      std::chrono::steady_clock::now()};
};

struct PendingClipboardMultipleRead {
  unsigned int sequence{0};
  xcb_window_t requestor{0};
  xcb_atom_t property{0};
  xcb_timestamp_t time{0};
};

struct PendingClipboardMultiple {
  xcb_window_t requestor{0};
  xcb_atom_t property{0};
  xcb_timestamp_t time{0};
  std::vector<xcb_atom_t> pairs; // target/property alternating
  std::size_t pending{0};
};

// A GetProperty issued after a SelectionNotify, not yet answered. Kept
// separate from PendingClipboardReceive (which describes the ConvertSelection
// side of the round-trip) since this tracks the second, inner request:
// SelectionNotify tells us the owner finished writing the property, but
// reading it back is its own request/reply pair. Deliberately polled via
// xcb_poll_for_reply() from process_events() rather than awaited with a
// blocking xcb_get_property_reply() — this fires from inside live X11
// event dispatch, and a window manager doing its own synchronous X11 work
// at the same moment (a server grab during reparenting, most commonly) can
// stall a blocking reply indefinitely, freezing the whole compositor for
// every client, not just the one waiting on this property. See
// set_window_title()'s comment (X11Backend_window.cpp) for the fuller
// version of this story — same class of hang, different call site.
struct PendingPropertyRead {
  unsigned int sequence{0}; // from the xcb_get_property_cookie_t
  xcb_atom_t property{0};   // deleted off selection_window once read
  PendingClipboardReceive req; // moved out of receive_queue already
  bool incr_chunk{false};
};

// Atom-name lookups for a TARGETS reply are asynchronous too: the clipboard
// event path must not block on an X server round-trip while a WM holds a
// server grab. The MIME list is published only after all requested names
// have been resolved.
struct PendingClipboardTargetName {
  unsigned int sequence{0};
  xcb_atom_t atom{0};
};

struct PendingClipboardTargets {
  std::vector<PendingClipboardTargetName> names_pending;
  std::vector<std::string> mimes;
  std::unordered_map<std::string, xcb_atom_t> mime_to_atom;
  bool has_utf8{false};
  bool has_plain_atom{false};
  bool has_string{false};
};

// Everything the clipboard half of the backend remembers between X events.
struct ClipboardState {
  xcb_window_t selection_window{0}; // hidden 1x1 window: owner + requestor

  xcb_atom_t atom_clipboard{0};
  xcb_atom_t atom_targets{0};
  xcb_atom_t atom_multiple{0};
  xcb_atom_t atom_incr{0};
  xcb_atom_t atom_atom_pair{0};
  xcb_atom_t atom_utf8_string{0};
  xcb_atom_t atom_text_plain_utf8{0}; // "text/plain;charset=utf-8"
  xcb_atom_t atom_ww_selection{0};    // our property name for ConvertSelection

  bool has_xfixes{false};
  uint8_t xfixes_first_event{0};

  // Outgoing: mime type -> the X11 atom we told the requestor we'd answer
  // to (both the mime type's own name-as-atom and the legacy aliases --
  // UTF8_STRING/STRING/TEXT -- when the mime type looks text-ish).
  std::vector<std::string> owned_mime_types;
  std::unordered_map<xcb_atom_t, std::string> outgoing_atom_to_mime;
  IWindowBackend::ClipboardSendCallback clipboard_send_cb;

  // Incoming: what the current non-us owner's TARGETS advertised, mapped
  // back to mime strings we understand, plus which atom to ask for each.
  std::unordered_map<std::string, xcb_atom_t> incoming_mime_to_atom;
  IWindowBackend::ClipboardOwnerChangedCallback clipboard_owner_changed_cb;

  // fd (pipe read end) -> in-flight SelectionRequest we're answering.
  std::unordered_map<int, PendingClipboardSend> pending_sends;
  std::unordered_map<uint64_t, PendingClipboardIncrSend> pending_incr_sends;
  std::optional<PendingClipboardIncrReceive> pending_incr_receive;
  bool pending_incr_chunk_notify{false};
  std::vector<PendingClipboardMultipleRead> pending_multiple_reads;
  std::unordered_map<uint64_t, PendingClipboardMultiple> pending_multiples;
  uint64_t next_multiple_id{1};
  // Requests we issued via ConvertSelection, processed strictly one at a
  // time (single `atom_ww_selection` property to receive into) -- extra
  // requests queue rather than needing per-request property atoms.
  std::deque<PendingClipboardReceive> receive_queue;
  bool receive_in_flight{false};
  // Set for the duration between firing the post-SelectionNotify
  // GetProperty and consuming its reply (polled from process_events(),
  // never blocked on) -- see PendingPropertyRead's comment.
  std::optional<PendingPropertyRead> pending_property_read;
  std::optional<PendingClipboardTargets> pending_clipboard_targets;
};

} // namespace ww
