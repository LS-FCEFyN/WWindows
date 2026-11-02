// X Render extension support, used for exactly one thing: correctly
// displaying dmabuf buffers whose channel order doesn't match this
// compositor's windows (DRM_FORMAT_ABGR8888/XBGR8888 — see
// linux_dmabuf/impl.hpp for why those show up at all).
//
// xcb_dri3_pixmap_from_buffers (X11Backend_dri3.cpp) and xcb_present_pixmap
// both treat pixel data as an opaque bag of bytes at a given depth/bpp —
// neither has any notion of which byte is "red". Render does: a Picture
// wraps a pixmap in a *declared* PictFormat (the shifts/masks describing
// which bits are which channel), and compositing from a Picture in one
// format onto a Picture in another converts between them. So a pixmap whose
// bytes are actually laid out red-then-green-then-blue can be told "no,
// treat byte 0 as blue" and Render will untangle it correctly when
// compositing onto the window's real (red-first) Picture.
//
// The catch: Render's wire protocol has no "define a new PictFormat"
// request — a client can only use a PictFormat id the server already
// advertises via RenderQueryPictFormats. init_render() queries that once
// and looks for one with exactly the shifts/masks a red/blue swap needs;
// every mainstream Xorg/Xwayland/Xvfb build's software Render fallback
// advertises one, but nothing guarantees a given server does, hence the
// fallback: if it isn't there, dmabuf import rejects the buffer instead of
// presenting it with the wrong colors, exactly as it already did for every
// BGR-order buffer before this file existed.
//
// Every window's destination Picture is created eagerly, alongside the
// window itself, in create_window_impl (X11Backend_window.cpp) — not
// lazily on first BGR-order present the way a more "obvious" design might
// do it. A lazy ensure_window_picture() would need to synchronously
// confirm the CreatePicture succeeded before compositing onto it, and that
// confirmation would happen from inside live Wayland-request dispatch —
// exactly the situation (see set_window_title's comment in
// X11Backend_window.cpp) where a blocking XCB call can freeze the whole
// compositor until whatever else the X server is doing (most commonly a
// window manager's server grab) clears. Creating it fire-and-forget at
// window-creation time sidesteps that entirely, the same way this
// compositor already handles Present's select_input: the PictFormat was
// already confirmed to exist for this exact visual/depth pairing by
// init_render(), and the window was just created by us, so failure here
// isn't a realistic outcome in practice.
#include "wwindows/adapters/x11/detail/x11_backend_impl.hpp"
#include <wwindows/support/log.hpp>

#include <algorithm>
#include <xcb/render.h>

namespace ww {

namespace {

// A PictFormat matching this is what a red/blue-swapped 24 or 32-bit
// buffer needs interpreted as: red in the lowest byte, green next, blue
// after that (DRM_FORMAT_ABGR8888/XBGR8888's "x/a:B:G:R" layout, low to
// high address, little-endian — see their definitions in drm_fourcc.h).
// Alpha is deliberately not checked: neither format actually gets
// alpha-blended (present_pixmap_via_render uses PictOpSrc, a straight
// replace, same as the raw Present path's non-blended copy for every other
// format this compositor imports), so whichever alpha mask the server
// happens to pair with this layout at a given depth is irrelevant.
bool is_bgr_layout(const xcb_render_directformat_t &d) {
  return d.red_shift == 0 && d.red_mask == 0xff && d.green_shift == 8 &&
        d.green_mask == 0xff && d.blue_shift == 16 && d.blue_mask == 0xff;
}

// The standard ARGB32 layout (what PictStandardARGB32 means): alpha in the
// top byte, then red, green, blue -- i.e. a little-endian wl_shm ARGB8888
// word. Unlike is_bgr_layout this one *does* check alpha, because the cursor
// path relies on the server blending with it.
bool is_argb_layout(const xcb_render_directformat_t &d) {
  return d.alpha_shift == 24 && d.alpha_mask == 0xff && d.red_shift == 16 &&
         d.red_mask == 0xff && d.green_shift == 8 && d.green_mask == 0xff &&
         d.blue_shift == 0 && d.blue_mask == 0xff;
}

} // namespace

bool X11Backend::init_render() const {
  auto &im = *impl_;

  const auto *render = xcb_get_extension_data(im.conn, &xcb_render_id);
  if (!render || !render->present) {
    WW_LOG_DEBUG("[x11] Render X extension not available");
    return false;
  }

  // RenderCreateCursor (X11Backend_cursor.cpp) needs Render 0.5 or newer, and
  // a client is expected to announce which version it speaks before using
  // anything past the base protocol -- the same handshake libXrender and
  // xcb-cursor perform. One extra round trip at startup, alongside the
  // PictFormat query below, and a failed reply is not fatal: the rest of
  // this file only needs the base protocol.
  if (auto *ver = xcb_render_query_version_reply(
          im.conn, xcb_render_query_version(im.conn, 0, 11), nullptr))
    ::free(ver);

  xcb_render_query_pict_formats_cookie_t cookie =
      xcb_render_query_pict_formats(im.conn);
  xcb_render_query_pict_formats_reply_t *reply =
      xcb_render_query_pict_formats_reply(im.conn, cookie, nullptr);
  if (!reply) {
    WW_LOG_WARN("[x11] RenderQueryPictFormats failed");
    return false;
  }

  // The flat "formats" list — every depth+layout combination the server's
  // Render implementation supports for a Picture on an arbitrary pixmap,
  // independent of any Visual. This is where a red/blue-swapped format
  // (not used by any real Visual on a sane server) would show up.
  xcb_render_pictforminfo_iterator_t fmt_it =
      xcb_render_query_pict_formats_formats_iterator(reply);
  for (; fmt_it.rem; xcb_render_pictforminfo_next(&fmt_it)) {
    const xcb_render_pictforminfo_t *f = fmt_it.data;
    if (f->type == XCB_RENDER_PICT_TYPE_DIRECT && f->depth == 32 &&
        is_argb_layout(f->direct) && !im.present.render_argb32_pictformat)
      im.present.render_argb32_pictformat = f->id;
    if (f->type != XCB_RENDER_PICT_TYPE_DIRECT || !is_bgr_layout(f->direct))
      continue;
    if (f->depth == 32 && !im.present.render_bgr32_pictformat)
      im.present.render_bgr32_pictformat = f->id;
    else if (f->depth == 24 && !im.present.render_bgr24_pictformat)
      im.present.render_bgr24_pictformat = f->id;
  }

  // The screens/depths/visuals hierarchy — this is how a Visual maps to
  // its PictFormat. We only ever create windows with one of two visuals
  // (screen->root_visual or visual32, see X11Backend_window.cpp), so grab
  // both up front rather than re-querying per window.
  xcb_render_pictscreen_iterator_t scr_it =
      xcb_render_query_pict_formats_screens_iterator(reply);
  for (; scr_it.rem; xcb_render_pictscreen_next(&scr_it)) {
    xcb_render_pictdepth_iterator_t depth_it =
        xcb_render_pictscreen_depths_iterator(scr_it.data);
    for (; depth_it.rem; xcb_render_pictdepth_next(&depth_it)) {
      xcb_render_pictvisual_iterator_t vis_it =
          xcb_render_pictdepth_visuals_iterator(depth_it.data);
      for (; vis_it.rem; xcb_render_pictvisual_next(&vis_it)) {
        const xcb_render_pictvisual_t *v = vis_it.data;
        if (v->visual == im.screen->root_visual)
          im.present.render_root_pictformat = v->format;
        else if (im.window.visual32 && v->visual == im.window.visual32)
          im.present.render_visual32_pictformat = v->format;
      }
    }
  }

  ::free(reply);

  im.present.has_render = im.present.render_root_pictformat || im.present.render_visual32_pictformat;
  WW_LOG_INFO("[x11] Render extension {}query complete (bgr32_fmt={}, "
             "bgr24_fmt={}, root_fmt={}, visual32_fmt={}, argb32_fmt={})",
             im.present.has_render ? "" : "available but no usable Render PictFormat — ",
             im.present.render_bgr32_pictformat, im.present.render_bgr24_pictformat,
             im.present.render_root_pictformat, im.present.render_visual32_pictformat,
             im.present.render_argb32_pictformat);
  return im.present.has_render;
}

void X11Backend::Impl::present_pixmap_via_render(Impl &im,
                                                 xcb_window_t window,
                                                 const RenderSource &src,
                                                 BufferRegion crop,
                                                 int32_t scale,
                                                 uint16_t stretch_width,
                                                 uint16_t stretch_height) {
  const auto it = im.present.window_pictures.find(window);
  if (it == im.present.window_pictures.end()) {
    WW_LOG_WARN("[x11] no destination Picture for window {} — can't "
               "composite/scale this dmabuf onto it", window);
    return;
  }

  // The crop is expressed in source-buffer pixels. For ordinary buffer_scale,
  // the target size is its logical size; for a stale frame, the explicit WM
  // target lets Render stretch the retained dmabuf snapshot without a CPU
  // readback. Render's transform maps destination coordinates back to source.
  const int32_t safe_scale = std::clamp<int32_t>(scale, 1, 32767);
  int32_t src_x = 0;
  int32_t src_y = 0;
  uint16_t source_w = src.width;
  uint16_t source_h = src.height;
  if (crop.declared()) {
    const int64_t left = std::max<int64_t>(crop.x, 0);
    const int64_t top = std::max<int64_t>(crop.y, 0);
    const int64_t right = std::min<int64_t>(
        static_cast<int64_t>(crop.x) + crop.width, src.width);
    const int64_t bottom = std::min<int64_t>(
        static_cast<int64_t>(crop.y) + crop.height, src.height);
    if (right <= left || bottom <= top)
      return;
    src_x = static_cast<int32_t>(left);
    src_y = static_cast<int32_t>(top);
    source_w = static_cast<uint16_t>(right - left);
    source_h = static_cast<uint16_t>(bottom - top);
  }

  const bool explicit_stretch = stretch_width != 0 && stretch_height != 0;
  const uint16_t dst_width = explicit_stretch
      ? stretch_width
      : static_cast<uint16_t>(
            (static_cast<uint32_t>(source_w) + static_cast<uint32_t>(safe_scale) - 1u) /
            static_cast<uint32_t>(safe_scale));
  const uint16_t dst_height = explicit_stretch
      ? stretch_height
      : static_cast<uint16_t>(
            (static_cast<uint32_t>(source_h) + static_cast<uint32_t>(safe_scale) - 1u) /
            static_cast<uint32_t>(safe_scale));
  if (dst_width == 0 || dst_height == 0)
    return;

  if (explicit_stretch || safe_scale > 1) {
    const auto fixed_ratio = [](uint32_t source, uint32_t destination) {
      const uint64_t fixed = (static_cast<uint64_t>(source) << 16u) / destination;
      return static_cast<xcb_render_fixed_t>(
          std::clamp<uint64_t>(fixed, 1u, static_cast<uint64_t>(INT32_MAX)));
    };
    const xcb_render_transform_t transform{
        fixed_ratio(source_w, dst_width), 0,
        static_cast<xcb_render_fixed_t>(std::min(src_x, 32767) << 16), 0,
        fixed_ratio(source_h, dst_height),
        static_cast<xcb_render_fixed_t>(std::min(src_y, 32767) << 16), 0, 0,
        static_cast<xcb_render_fixed_t>(1 << 16)};
    xcb_render_set_picture_transform(im.conn, src.picture, transform);
    xcb_render_set_picture_filter(im.conn, src.picture, 7, "nearest", 0,
                                  nullptr);
    xcb_render_composite(im.conn, static_cast<uint8_t>(XCB_RENDER_PICT_OP_SRC),
                         src.picture, XCB_RENDER_PICTURE_NONE, it->second,
                         0, 0, 0, 0, 0, 0, dst_width, dst_height);
    const xcb_render_transform_t identity{1 << 16, 0, 0, 0, 1 << 16,
                                          0, 0, 0, 1 << 16};
    xcb_render_set_picture_transform(im.conn, src.picture, identity);
  } else {
    xcb_render_composite(im.conn, static_cast<uint8_t>(XCB_RENDER_PICT_OP_SRC),
                         src.picture, XCB_RENDER_PICTURE_NONE, it->second,
                         static_cast<int16_t>(std::min(src_x, 32767)),
                         static_cast<int16_t>(std::min(src_y, 32767)), 0, 0, 0,
                         0, dst_width, dst_height);
  }
  xcb_flush(im.conn);
}

} // namespace ww
