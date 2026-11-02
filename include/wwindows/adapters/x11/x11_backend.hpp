#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <wwindows/ports/window_backend.hpp>
#include <wwindows/support/error.hpp>

// Forward declare XCB types to avoid leaking them to all including files
struct xcb_connection_t;
struct xcb_screen_t;

namespace ww {

class EventLoop;

// X11/XCB implementation of IWindowBackend. This is the only place in the
// codebase that knows about XCB, DRI3, or the Present extension — core/ and
// input/ talk to it exclusively through the IWindowBackend interface.
//
// Callers that specifically need raw X11/XCB access (currently just the
// WWindows bootstrap, to read initial screen dimensions) may still use the
// extra, non-virtual members below; those are deliberately kept off
// IWindowBackend since they are not portable to another backend.
class X11Backend final : public IWindowBackend {
public:
  ~X11Backend() override;
  X11Backend(const X11Backend &) = delete;
  X11Backend &operator=(const X11Backend &) = delete;
  X11Backend(X11Backend &&) = delete;
  X11Backend &operator=(X11Backend &&) = delete;

  // Creates the X11 connection and registers its fd with the EventLoop.
  static Result<std::unique_ptr<X11Backend>> create(EventLoop &loop);

  // -----------------------------------------------------------------------
  // X11-specific extras (not part of IWindowBackend)
  // -----------------------------------------------------------------------
  [[nodiscard]] xcb_connection_t *connection() const;
  [[nodiscard]] xcb_screen_t *screen() const;

  // The X screen as the server has it. X owns every number here; the backend
  // only remembers the last thing it was told. Not part of IWindowBackend:
  // it is the window system's screen, which the composition root turns into
  // whatever a compositor protocol wants to say about an output.
  struct ScreenGeometry {
    int32_t width_px{0};
    int32_t height_px{0};
    int32_t width_mm{0};
    int32_t height_mm{0};
    uint32_t refresh_mhz{0}; // 0: the server cannot say (no RandR, or it reports 0)
    friend bool operator==(const ScreenGeometry &, const ScreenGeometry &) = default;
  };
  // Current as of the last RandR notification (read from the connection setup
  // at startup, so never stale the way xcb_screen_t's size is).
  [[nodiscard]] ScreenGeometry screen_geometry() const;
  // Called when the server changes the screen (RRScreenChangeNotify): a
  // resolution change, a monitor being added or removed, a DPI change.
  // Replaces any previous callback. Not called for notifications that leave
  // the geometry as it was.
  void set_screen_changed_callback(
      std::function<void(const ScreenGeometry &)> cb);

  // Reads a string property (type STRING or UTF8_STRING) off the X root
  // window -- how a window manager advertises things like i3's IPC socket
  // path in I3_SOCKET_PATH. Empty optional if the property does not exist or
  // is not a string. Makes blocking round-trips to the server, so call it at
  // startup only, never from inside event dispatch (see set_window_title's
  // comment in X11Backend_window.cpp for why that matters).
  [[nodiscard]] std::optional<std::string>
  root_string_property(const char *name) const;

  // Registers interest in a single named root-window property, invoking cb
  // whenever that property is set, changed, or removed on the root window
  // from this call onward. cb is handed the property's new value (a
  // deletion delivers std::nullopt, same as root_string_property() finding
  // nothing) rather than nothing at all, so a caller doesn't need a
  // redundant root_string_property() call of its own inside the callback
  // just to find out what changed. Does NOT fire for whatever the property's
  // value already is at the moment this is called -- a caller that wants
  // that too should read it first with root_string_property(), exactly as
  // application.cpp does for I3_SOCKET_PATH.
  //
  // Exists for exactly the class of race root_string_property() cannot
  // defend against on its own: some other process (typically a window
  // manager) sets this property from its own independent startup sequence,
  // at some point not guaranteed to be before or after this one's -- launch
  // this process from that window manager's own config/exec and the two
  // startups are directly racing each other. A single synchronous read
  // taken too early just comes back empty, with nothing left to ever
  // correct it, unless the caller also does this. At most one callback per
  // property name; a second registration for the same name replaces the
  // first.
  void watch_root_property(const char *name,
                           std::function<void(std::optional<std::string>)> cb);

  // True when both DRI3 and the X Present extension are available.
  // Call after create(); checked by LinuxDmabuf before advertising dmabuf.
  [[nodiscard]] bool init_dri3_present() const;
  [[nodiscard]] bool has_dri3() const;

  // True when the X Render extension is available *and* the server
  // advertises at least one usable red/blue-swapped PictFormat — i.e.
  // import_dmabuf(..., bgr_order=true) will actually work rather than
  // reject the buffer. Call once, after init_dri3_present() succeeds
  // (queries and caches the PictFormats it needs). A false return isn't
  // fatal: native-order dmabuf import and every other code path are
  // unaffected, only ABGR8888/XBGR8888 buffers get rejected instead of
  // converted. See X11Backend_render.cpp.
  [[nodiscard]] bool init_render() const;

  // -----------------------------------------------------------------------
  // IWindowBackend
  // -----------------------------------------------------------------------
  [[nodiscard]] uint32_t create_window(uint16_t width,
                                       uint16_t height) override;
  [[nodiscard]] uint32_t create_child_window(uint32_t parent, int16_t x,
                                             int16_t y, uint16_t width,
                                             uint16_t height) override;
  void move_window(uint32_t window, int16_t x, int16_t y) override;
  void restack_window(uint32_t window, uint32_t sibling, bool above) override;
  void destroy_window(uint32_t window) override;
  void forget_window(uint32_t window) override;
  void map_window(uint32_t window) override;
  void unmap_window(uint32_t window) override;
  void repaint_window(uint32_t window, BufferRegion src = {},
                      int32_t buffer_scale = 0,
                      FramePresentedCallback on_presented = {}) override;
  void clear_retained_frame(uint32_t window) override;
  void resize_window(uint32_t window, uint16_t width,
                     uint16_t height) override;
  void set_window_title(uint32_t window, const char *utf8_title) override;
  void set_window_class(uint32_t window, const char *instance_name,
                        const char *class_name) override;
  void set_window_size_limits(uint32_t window,
                              const WindowSizeLimits &limits) override;
  void set_window_transient_for(uint32_t window, uint32_t parent) override;
  void set_window_client_process(uint32_t window, uint32_t pid) override;

  // _NET_WM_STATE_FULLSCREEN / _NET_WM_STATE_MAXIMIZED_{VERT,HORZ}, sent
  // to the window manager as an EWMH client message. See
  // X11Backend_window.cpp.
  void set_window_state(uint32_t window, WindowState state,
                        bool enabled) override;

  // _NET_WM_MOVERESIZE, sent to the window manager the same way
  // set_window_state sends _NET_WM_STATE. See X11Backend_window.cpp.
  void request_interactive_move(uint32_t window) override;
  void request_interactive_resize(uint32_t window, ResizeEdge edge) override;

  // WM_CHANGE_STATE(IconicState), per ICCCM 4.1.4. See
  // X11Backend_window.cpp.
  void minimize_window(uint32_t window) override;

  // wl_pointer.set_cursor as a real X11 cursor: the client's pixels are
  // uploaded to a depth-32 pixmap, wrapped in a Render picture and turned
  // into a cursor with RenderCreateCursor (what libXcursor does), then
  // applied with ChangeWindowAttributes(CWCursor). See X11Backend_cursor.cpp.
  [[nodiscard]] uint32_t create_cursor(const CursorImage &image) override;
  void free_cursor(uint32_t cursor) override;
  [[nodiscard]] uint32_t blank_cursor() override;
  void set_window_cursor(uint32_t window, uint32_t cursor) override;

  void set_input_shape(uint32_t window, bool has_region,
                       const std::vector<InputRect> &add_rects,
                       const std::vector<InputRect> &sub_rects) override;

  // xcb_query_keymap, translated to a list of set keycodes. See the port
  // doc comment for why this exists.
  [[nodiscard]] std::vector<uint8_t> query_pressed_keycodes() override;

  // The real X server keymap via libxkbcommon-x11, serialized. See the
  // port doc comment.
  [[nodiscard]] std::optional<std::string> keyboard_keymap_string() override;
  [[nodiscard]] std::optional<KeyRepeat> keyboard_repeat() override;

  void present_pixels(uint32_t window, uint16_t width, uint16_t height,
                      uint32_t stride, const void *pixels,
                      BufferRegion src = {},
                      FramePresentedCallback on_presented = {}) override;

  // Import a dmabuf fd as an X11 Pixmap via DRI3.
  // Takes ownership of fd (closes it on both success and failure).
  // Calls on_imported with 0 (XCB_NONE) if DRI3 is unavailable or the
  // import fails — see IWindowBackend::import_dmabuf for why this is
  // asynchronous. bgr_order=true (ABGR8888/XBGR8888) additionally
  // creates a Render Picture on the imported pixmap in the matching
  // swapped-channel PictFormat once the import is confirmed, so
  // present_pixmap() can composite instead of blit — see
  // X11Backend_render.cpp. Import fails (same as DRI3 being unavailable)
  // if Render, or a usable PictFormat for this depth, isn't there.
  void import_dmabuf(int fd, uint32_t window, uint16_t width,
                     uint16_t height, uint8_t depth, uint8_t bpp,
                     uint16_t stride, uint64_t modifier, bool bgr_order,
                     DmabufImportCallback on_imported) override;

  // Present a DRI3 pixmap to the window. Pixmaps imported with
  // bgr_order=true composite through Render (X11Backend_render.cpp);
  // everything else goes through the fast xcb_present_pixmap path, same
  // as before.
  void present_pixmap(uint32_t window, uint32_t pixmap,
                      BufferRegion src = {}, int32_t buffer_scale = 1,
                      FramePresentedCallback on_presented = {},
                      FramePresentedCallback on_source_consumed = {},
                      uint16_t stretch_width = 0,
                      uint16_t stretch_height = 0) override;
  void repaint_window_scaled(uint32_t window, uint16_t width,
                             uint16_t height,
                             FramePresentedCallback on_presented = {}) override;

  // Free a pixmap returned by import_dmabuf, and any Render Picture that
  // came with it.
  // Called from ImportedBuffer's resource destructor.
  void present_nv12(uint32_t window, uint16_t width, uint16_t height,
                    int fd_y, uint32_t offset_y, uint32_t stride_y,
                    int fd_uv, uint32_t offset_uv, uint32_t stride_uv,
                    BufferRegion src = {}, int32_t buffer_scale = 1,
                    FramePresentedCallback on_presented = {},
                    uint16_t stretch_width = 0,
                    uint16_t stretch_height = 0) override;

  void free_pixmap(uint32_t pixmap) override;

  // dev_t of the DRI3-opened DRM device fd (im.drm_fd), via fstat().
  // std::nullopt when DRI3 never initialised (has_dri3() == false) or the
  // fstat() call itself fails.
  [[nodiscard]] std::optional<dev_t> dmabuf_main_device() const override;

  void set_resize_callback(ResizeCallback cb) override;
  void set_wm_state_callback(WmStateCallback cb) override;
  void set_close_callback(CloseCallback cb) override;
  void set_input_callbacks(InputCallbacks cbs) override;
  void set_events_processed_callback(std::function<void()> cb) override;

  void set_clipboard_owner(std::vector<std::string> mime_types) override;
  void set_clipboard_send_callback(ClipboardSendCallback cb) override;
  void set_clipboard_owner_changed_callback(
      ClipboardOwnerChangedCallback cb) override;
  void request_clipboard_data(const std::string &mime_type,
                              ClipboardDataCallback on_data) override;

private:
  X11Backend();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ww
