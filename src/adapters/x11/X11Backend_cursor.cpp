// wl_pointer.set_cursor <-> X11 pointer cursors.
//
// A Wayland client does not pick a cursor *shape*, it hands the compositor a
// wl_surface containing the cursor's pixels. X11 has the same idea, just
// spelled differently: a server-side cursor object built from an image
// (RenderCreateCursor, the call libXcursor makes for every themed ARGB
// cursor) and attached to a window with CWCursor. While the pointer is over
// that window the X server shows that image -- moving it, accelerating it
// and compositing it itself -- so there is nothing for this compositor to
// draw or keep in step with the pointer.
//
// The earlier design mapped a click-through overlay window that followed the
// pointer. It was never mapped (the host server's own arrow would have shown
// through as a second cursor), so a client's cursor never reached the screen
// at all and every Wayland window showed the X server's default arrow
// whatever the client asked for.

#include "wwindows/adapters/x11/detail/x11_backend_impl.hpp"
#include <wwindows/support/log.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <xcb/render.h>
#include <xcb/xproto.h>

namespace ww {

namespace {

// Beyond this, servers fall back to software cursors or refuse outright
// (XQueryBestCursor reports 256x256 on every mainstream driver), and no
// real cursor theme ships anything near it.
constexpr uint16_t kMaxCursorSize = 256;

// PutImage's fixed part: 24 bytes of request header, rounded up generously.
constexpr std::size_t kPutImageOverhead = 32;

} // namespace

uint32_t X11Backend::create_cursor(const CursorImage &image) {
  auto &im = *impl_;

  // render_argb32_pictformat is only ever set by a successful init_render(),
  // so non-zero also means the Render extension is there and was queried.
  if (!im.present.render_argb32_pictformat) {
    WW_LOG_WARN("[x11] cursor not created: the server has no Render ARGB32 "
                "format, so it cannot build an alpha-blended cursor");
    return 0;
  }
  if (!image.argb || image.width == 0 || image.height == 0 ||
      image.width > kMaxCursorSize || image.height > kMaxCursorSize ||
      image.stride < static_cast<uint32_t>(image.width) * 4u) {
    WW_LOG_DEBUG("[x11] cursor not created: unusable image ({}x{}, stride={})",
                 image.width, image.height, image.stride);
    return 0;
  }

  const uint16_t w = image.width;
  const uint16_t h = image.height;
  // A hotspot outside the image is a client bug the server would reject
  // (BadMatch); pin it to the nearest pixel instead of losing the cursor.
  const uint16_t hx = static_cast<uint16_t>(
      std::clamp<int32_t>(image.hotspot_x, 0, static_cast<int32_t>(w) - 1));
  const uint16_t hy = static_cast<uint16_t>(
      std::clamp<int32_t>(image.hotspot_y, 0, static_cast<int32_t>(h) - 1));

  // Tightly pack the rows: PutImage on a 32-bpp ZPixmap has no stride field,
  // and the source stride may include padding.
  const std::size_t row_bytes = static_cast<std::size_t>(w) * 4u;
  std::vector<uint8_t> tight(row_bytes * h);
  const auto *src = static_cast<const uint8_t *>(image.argb);
  for (uint16_t y = 0; y < h; ++y)
    std::memcpy(tight.data() + static_cast<std::size_t>(y) * row_bytes,
                src + static_cast<std::size_t>(y) * image.stride, row_bytes);

  const xcb_pixmap_t pixmap = xcb_generate_id(im.conn);
  const xcb_gcontext_t gc = xcb_generate_id(im.conn);
  const xcb_render_picture_t picture = xcb_generate_id(im.conn);
  const xcb_cursor_t cursor = xcb_generate_id(im.conn);

  xcb_create_pixmap(im.conn, 32, pixmap, im.screen->root, w, h);
  xcb_create_gc(im.conn, gc, pixmap, 0, nullptr);

  // Upload in horizontal strips so a request never exceeds what the server
  // accepts, with or without BIG-REQUESTS (xcb_get_maximum_request_length
  // reports the limit in 4-byte units).
  const std::size_t max_request_bytes =
      static_cast<std::size_t>(xcb_get_maximum_request_length(im.conn)) * 4u;
  const std::size_t budget = max_request_bytes > kPutImageOverhead + row_bytes
                                 ? max_request_bytes - kPutImageOverhead
                                 : row_bytes;
  const uint16_t rows_per_strip = static_cast<uint16_t>(
      std::clamp<std::size_t>(budget / row_bytes, 1, h));
  for (uint16_t y = 0; y < h; y = static_cast<uint16_t>(y + rows_per_strip)) {
    const uint16_t rows = std::min<uint16_t>(rows_per_strip,
                                             static_cast<uint16_t>(h - y));
    xcb_put_image(im.conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc, w, rows, 0,
                  static_cast<int16_t>(y), 0, 32,
                  static_cast<uint32_t>(row_bytes * rows),
                  tight.data() + static_cast<std::size_t>(y) * row_bytes);
  }

  xcb_render_create_picture(im.conn, picture, pixmap,
                            im.present.render_argb32_pictformat, 0, nullptr);
  xcb_render_create_cursor(im.conn, cursor, picture, hx, hy);

  // The cursor keeps its own copy of the image; the staging objects are done.
  xcb_render_free_picture(im.conn, picture);
  xcb_free_gc(im.conn, gc);
  xcb_free_pixmap(im.conn, pixmap);
  xcb_flush(im.conn);

  WW_LOG_DEBUG("[x11] cursor {} created ({}x{}, hotspot=({},{}))", cursor, w,
               h, hx, hy);
  return cursor;
}

void X11Backend::free_cursor(uint32_t cursor) {
  auto &im = *impl_;
  // The blank cursor is shared and owned by the backend; a caller that frees
  // it by mistake must not leave blank_cursor() handing out a dead id.
  if (cursor == 0 || cursor == im.blank_cursor_id)
    return;
  xcb_free_cursor(im.conn, cursor);
  xcb_flush(im.conn);
}

uint32_t X11Backend::blank_cursor() {
  auto &im = *impl_;
  if (im.blank_cursor_id != XCB_CURSOR_NONE)
    return im.blank_cursor_id;

  static constexpr uint32_t kTransparentPixel = 0;
  im.blank_cursor_id = create_cursor(CursorImage{.width = 1,
                                                 .height = 1,
                                                 .hotspot_x = 0,
                                                 .hotspot_y = 0,
                                                 .stride = 4,
                                                 .argb = &kTransparentPixel});
  return im.blank_cursor_id;
}

void X11Backend::set_window_cursor(uint32_t window, uint32_t cursor) {
  auto &im = *impl_;
  if (window == 0)
    return;
  // cursor == 0 is XCB_CURSOR_NONE: "use the parent's cursor", which is
  // exactly the default this port promises.
  const uint32_t value = cursor;
  xcb_change_window_attributes(im.conn, static_cast<xcb_window_t>(window),
                               XCB_CW_CURSOR, &value);
  xcb_flush(im.conn);
}

} // namespace ww
