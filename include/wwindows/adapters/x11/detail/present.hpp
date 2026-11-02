#pragma once
// DRI3 / Present / Render state owned by X11Backend::Impl.
//
// Covers the zero-copy dmabuf import pipeline (DRI3 PixmapFromBuffers), frame
// presentation (Present), BGR channel correction and integer scaling
// (Render), and the retained-frame cache an Expose event replays. The state
// machines that drive it are the static methods on X11Backend::Impl
// (X11Backend_dri3.cpp, _present.cpp, _render.cpp, _events.cpp).
//
// Pure data: no methods.

#include "wwindows/ports/window_backend.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xcb/present.h>
#include <xcb/render.h>
#include <xcb/xcb.h>

namespace ww {

// One outstanding DRI3 PixmapFromBuffers import (see
// X11Backend::import_dmabuf's implementation comment for the full
// rationale). `request_sequence` is the checked import request's own
// sequence — matched against incoming X errors in dispatch_one() to
// detect failure as soon as it's reported. `fence_sequence` is a cheap
// xcb_get_input_focus issued immediately after it purely as a
// synchronization point: X11 guarantees strict per-connection request
// ordering, so once the fence's reply arrives (polled from
// poll_pending_dmabuf_imports(), never blocked on), the server has
// necessarily already finished processing (and reporting any error for)
// the import — silence by then means it succeeded. Removed from
// pending_dmabuf_imports the moment either signal resolves it, whichever
// comes first.
//
// source_pictformat/bgr_order/width/height carry what finish_dmabuf_import()
// needs to set up a Render source Picture for channel correction or scaling (see
// X11Backend_render.cpp) — nothing to do with the import's own
// success/failure, just state that would otherwise have to be looked back
// up from the pixmap id alone.
struct PendingDmabufImport {
  unsigned int request_sequence{0};
  unsigned int fence_sequence{0};
  uint32_t pixmap{0};
  bool bgr_order{false};
  xcb_render_pictformat_t bgr_pictformat{0}; // only meaningful if bgr_order
  xcb_render_pictformat_t source_pictformat{0};
  uint16_t width{0};
  uint16_t height{0};
  uint8_t depth{0};
  IWindowBackend::DmabufImportCallback callback;
};

// A Render Picture wrapping a dmabuf-imported pixmap whose channel order
// doesn't match this window's visual (ABGR8888/XBGR8888) — created once in
// finish_dmabuf_import() on successful import, consumed every frame by
// present_pixmap()'s Render path, released in free_pixmap(). Width/height
// are the pixmap's own — needed by the composite call, which
// present_pixmap() otherwise has no way to know (it's only ever handed the
// pixmap id). See X11Backend_render.cpp.
struct RenderSource {
  xcb_render_picture_t picture{0};
  uint16_t width{0};
  uint16_t height{0};
  bool bgr_order{false};
  xcb_render_pictformat_t pictformat{0};
};

struct PixmapMetadata {
  uint16_t width{0};
  uint16_t height{0};
  uint8_t depth{0};
  xcb_render_pictformat_t pictformat{0};
  bool bgr_order{false};
};

struct RetainedFrame {
  bool is_pixmap{false};
  uint16_t width{0};
  uint16_t height{0};
  uint32_t stride{0};
  BufferRegion region{};
  int32_t buffer_scale{1};
  std::vector<uint8_t> pixels;
  uint32_t pixmap{0};
  // NV12 is converted to XRGB for normal presentation, but preserve a full
  // resolution converted source so a scale-only commit can re-render it.
  std::vector<uint8_t> canonical_pixels;
  uint16_t canonical_width{0};
  uint16_t canonical_height{0};
  uint32_t canonical_stride{0};
  BufferRegion canonical_region{};
  int32_t canonical_scale{1};
};

struct PendingFrameFence {
  uint32_t window{0};
  unsigned int sequence{0};
  IWindowBackend::FramePresentedCallback callback;
  // Source-consumed fences protect wl_buffer release, not frame callbacks.
  // They must still run if the target X11 window is destroyed.
  bool cancel_on_window_destroy{true};
};

struct PendingPresentCallback {
  uint32_t window{0};
  IWindowBackend::FramePresentedCallback callback;
};

struct PresentState {
  bool has_dri3_present{false};
  uint8_t present_opcode{0};
  uint32_t next_present_serial{1};
  int drm_fd{-1};

  // --- Render extension (BGR channel correction and integer scaling) ------
  // See X11Backend_render.cpp. render_bgr32/24_pictformat are 0 until
  // init_render() finds a matching PictFormat (or fails to); has_render is
  // true when a usable source/destination format pair exists. render_root_pictformat /
  // render_visual32_pictformat are the destination formats matching this
  // screen's two window visuals (root_visual / WindowBackendState::visual32)
  // -- needed to create each window's destination Picture eagerly at
  // creation time (see create_window_impl in X11Backend_window.cpp).
  bool has_render{false};
  xcb_render_pictformat_t render_bgr32_pictformat{0};
  xcb_render_pictformat_t render_bgr24_pictformat{0};
  xcb_render_pictformat_t render_root_pictformat{0};
  xcb_render_pictformat_t render_visual32_pictformat{0};
  // Standard premultiplied ARGB32 PictFormat (depth 32, alpha<<24, then
  // R, G, B in the usual order) -- the one source format RenderCreateCursor
  // wants for a client's cursor pixels. Looked up from the flat format list,
  // independent of any Visual. 0 when the server offers none.
  xcb_render_pictformat_t render_argb32_pictformat{0};

  // Destination Picture per window, created alongside the window itself
  // (create_window_impl) rather than lazily on first BGR-order present --
  // creating it later would mean an XCB round-trip from inside live
  // dispatch to confirm it, same class of hang as everything else this
  // backend works around; see create_window_impl's comment. Freed in
  // destroy_window()/forget_window() alongside the GCs. Absence (window
  // never got an entry, e.g. has_render was false) means "can't composite
  // a BGR-order dmabuf onto this window" -- see present_pixmap_via_render().
  std::unordered_map<uint32_t, xcb_render_picture_t> window_pictures;
  // Source Pictures are retained for BGR channel correction and scaled
  // presentation. Native-order scale-1 buffers still use the fast Present
  // path; the Render picture is only used when necessary.
  std::unordered_map<uint32_t, RenderSource> render_source_pictures;
  std::unordered_map<uint32_t, PixmapMetadata> pixmap_metadata;
  // Independent copy of the latest image presented to each X11 window. An
  // Expose event replays this cache rather than re-reading a client-owned
  // wl_buffer whose resource may already have been destroyed or reused.
  std::unordered_map<uint32_t, RetainedFrame> retained_frames;
  std::unordered_set<uint32_t> present_in_flight_pixmaps;
  std::unordered_set<uint32_t> retired_retained_pixmaps;
  std::unordered_map<uint32_t, uint32_t> retired_retained_pixmap_windows;

  std::unordered_map<uint32_t, xcb_present_event_t> present_events;
  std::unordered_map<uint32_t, PendingPresentCallback> present_frame_callbacks;
  std::vector<PendingFrameFence> pending_frame_fences;

  // In-flight DRI3 imports -- see PendingDmabufImport's comment. A vector
  // is fine: imports happen per unique buffer object (typically a handful
  // per client, not per frame), so this stays small; entries are removed
  // via swap-and-pop, so order doesn't matter.
  std::vector<PendingDmabufImport> pending_dmabuf_imports;
};

} // namespace ww
