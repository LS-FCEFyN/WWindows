#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <sys/types.h> // dev_t
#include <vector>

namespace ww {

// A single rectangle in surface-local coordinates, used to build up an
// input region (wl_region as consumed by wl_surface.set_input_region).
struct InputRect {
  int16_t x{0};
  int16_t y{0};
  uint16_t width{0};
  uint16_t height{0};
};

// The sub-rectangle of a client buffer that is the window *proper*, in
// buffer-local pixels — xdg_surface.set_window_geometry.
//
// A client is free to attach a buffer larger than the size it was
// configured with, and most toolkits do: the extra ring of pixels is a
// drop shadow and an invisible resize border, which the client draws but
// which is emphatically *not* part of the window. set_window_geometry is
// how it says which part is. A compositor that ignores it blits the shadow
// onto the screen and pushes the real window down and to the right by the
// margin, cropping the opposite edges off against the window bounds.
//
// So: `x`/`y` is the point in the buffer that must land at the window's
// top-left, and `width`/`height` is how much of it is the window. A
// default-constructed value means "no geometry declared" — present the
// whole buffer, which is what every surface without an xdg_surface role
// (subsurfaces) does.
struct BufferRegion {
  int16_t x{0};
  int16_t y{0};
  uint16_t width{0};
  uint16_t height{0};

  [[nodiscard]] constexpr bool declared() const noexcept {
    return width != 0 && height != 0;
  }
};

// The size limits a toplevel declares through xdg_toplevel.set_min_size /
// set_max_size. Zero in a dimension means "no limit" there, exactly as in the
// protocol; a backend translates that into whatever its window system uses
// to say the same thing (WM_NORMAL_HINTS on X11).
struct WindowSizeLimits {
  int32_t min_width{0};
  int32_t min_height{0};
  int32_t max_width{0};
  int32_t max_height{0};

  [[nodiscard]] constexpr bool any() const noexcept {
    return min_width > 0 || min_height > 0 || max_width > 0 || max_height > 0;
  }
};

// A window-manager state a client can ask for through xdg_toplevel
// (set_fullscreen / set_maximized). Backends with no such concept may
// treat this as a no-op; the client simply never gets the state back in
// its next configure.
enum class WindowState {
  Fullscreen,
  Maximized,
};

// A pointer image handed to the window system: what a client's cursor
// surface (wl_pointer.set_cursor) looks like once flattened to pixels.
//
// `argb` points at `height` rows of `stride` bytes each, 32 bits per pixel,
// little-endian ARGB with *premultiplied* alpha -- exactly wl_shm's
// ARGB8888, and the layout every window system's own cursor path expects, so
// no conversion is needed on the way through. The backend copies the pixels
// during create_cursor(); the pointer need only stay valid for the call.
// The hotspot is in image pixels, measured from the top-left corner.
struct CursorImage {
  uint16_t width{0};
  uint16_t height{0};
  int16_t hotspot_x{0};
  int16_t hotspot_y{0};
  uint32_t stride{0};
  const void *argb{nullptr};
};

// xdg_toplevel.resize's edge argument, forwarded with the same numeric
// values xdg_toplevel_resize_edge itself uses on the wire, so a caller can
// pass the protocol's own value straight through with no translation
// table of its own (checked with a static_assert next to
// XdgSurfaceData's use of it in Toplevel.cpp). `None` is a legal value on
// the wire -- a client resizing without pinning down which edge grew --
// with no obviously correct interpretation; a backend maps it to
// whatever its own window-manager protocol treats as a sane default for
// "no edge specified".
enum class ResizeEdge : uint32_t {
  None = 0,
  Top = 1,
  Bottom = 2,
  Left = 4,
  TopLeft = 5,
  BottomLeft = 6,
  Right = 8,
  TopRight = 9,
  BottomRight = 10,
};

// Abstract presentation/windowing backend.
//
// SurfaceManager and InputTranslator are written against this interface only
// — never against a concrete backend such as X11Backend. That means the
// entire core/ and input/ layer is backend-agnostic: swapping X11 for a
// different presentation backend (a headless backend for tests, a future
// native Windows/macOS backend, a DRM/KMS backend, ...) only requires a new
// class that implements IWindowBackend. No change to core/ or input/.
//
// Concrete backends (e.g. platform/xwindows/X11Backend) may expose extra,
// backend-specific functionality beyond this interface, but anything used by
// SurfaceManager or InputTranslator must live here.
class IWindowBackend {
public:
  virtual ~IWindowBackend() = default;

  // --- Window lifecycle ----------------------------------------------------
  [[nodiscard]] virtual uint32_t create_window(uint16_t width,
                                               uint16_t height) = 0;

  // Subsurfaces (wl_subcompositor / wl_subsurface): create a window nested
  // inside `parent`, positioned in parent-relative coordinates. On X11 this
  // is a genuine child window — the server clips it to the parent's bounds
  // and moves it along with the parent automatically, which is exactly
  // wl_subsurface's semantics with no compositing pass required. Backends
  // without a native child-window concept may implement this as a top-level
  // window they manually keep aligned with the parent.
  [[nodiscard]] virtual uint32_t create_child_window(uint32_t parent,
                                                     int16_t x, int16_t y,
                                                     uint16_t width,
                                                     uint16_t height) = 0;
  // Reposition a window in its parent's coordinate space (subsurfaces only —
  // top-level position is left to the window manager).
  virtual void move_window(uint32_t window, int16_t x, int16_t y) = 0;
  // Restack a window relative to a sibling under the same parent.
  // sibling == 0 restacks relative to all siblings (top/bottom of stack).
  virtual void restack_window(uint32_t window, uint32_t sibling,
                              bool above) = 0;

  virtual void destroy_window(uint32_t window) = 0;
  // Drops backend-side bookkeeping for a window without sending any
  // protocol request to destroy it. For subsurfaces: X11 destroys child
  // windows automatically when their parent is destroyed, so when a parent
  // surface goes away this is used instead of destroy_window() on each
  // child — issuing a second, explicit destroy on an id the server has
  // already invalidated would just generate a BadWindow error.
  virtual void forget_window(uint32_t window) = 0;
  virtual void map_window(uint32_t window) = 0;
  virtual void unmap_window(uint32_t window) = 0;
  // Restore the last committed image without consulting the client's
  // wl_buffer resource. Backends own the retained presentation image.
  virtual void clear_retained_frame(uint32_t window) = 0;
  virtual void resize_window(uint32_t window, uint16_t width,
                             uint16_t height) = 0;
  virtual void set_window_title(uint32_t window, const char *utf8_title) = 0;

  // --- Window identity and hints -------------------------------------------
  // What a window manager needs to know about a toplevel *as a window* --
  // which application it belongs to, how big it may be, what it is a dialog
  // of, who owns it. On X11 these are ICCCM/EWMH properties, and they are
  // only useful if they are in place before the window is first mapped:
  // that is the moment a window manager decides where the window goes
  // (workspace assignment, floating rules, size) and it does not always
  // revisit the decision when a property changes later. So callers set
  // them between create_window() and map_window(), and may update them
  // afterwards. All of them are safe to call from inside live Wayland
  // request dispatch -- none may block on a round-trip to the window
  // system (see set_window_title's implementation for why that matters).

  // xdg_toplevel.set_app_id. `instance_name` / `class_name` are the two
  // halves of X11's WM_CLASS; a Wayland app_id has no such split, so callers
  // pass it as both. Null or empty removes the class.
  virtual void set_window_class(uint32_t window, const char *instance_name,
                                const char *class_name) = 0;

  // xdg_toplevel.set_min_size / set_max_size, combined. A default-constructed
  // WindowSizeLimits (no limits at all) removes the hints.
  virtual void set_window_size_limits(uint32_t window,
                                      const WindowSizeLimits &limits) = 0;

  // xdg_toplevel.set_parent: `window` is a transient (dialog) of `parent`.
  // parent == 0 makes it an independent toplevel again.
  virtual void set_window_transient_for(uint32_t window, uint32_t parent) = 0;

  // The operating-system process that owns the window (_NET_WM_PID on X11,
  // together with the host name it runs on). Only meaningful for a client
  // running on the same machine as the compositor, which every Wayland
  // client is by construction.
  virtual void set_window_client_process(uint32_t window, uint32_t pid) = 0;

  // --- Pointer cursors ---------------------------------------------------
  // wl_pointer.set_cursor, translated to the window system's *own* pointer
  // cursor mechanism (an X11 cursor object, applied to a window) rather than
  // drawn by the compositor. The window system keeps owning the pointer: it
  // moves it, hardware-accelerates it and composites it for free, and the
  // image the client chose simply replaces the arrow it would otherwise show
  // over that window.
  //
  // A cursor is an opaque, immutable handle. Handles are cheap to keep and to
  // apply to many windows; the backend reference-counts the underlying
  // object, so free_cursor() on a handle that some window still shows is
  // safe -- that window keeps the image until it is given another one.

  // Builds a cursor from `image`. Returns 0 when the backend cannot (no
  // Render extension, an image larger than the window system supports, ...);
  // callers treat 0 as "keep whatever cursor is showing".
  [[nodiscard]] virtual uint32_t create_cursor(const CursorImage &image) = 0;
  virtual void free_cursor(uint32_t cursor) = 0;

  // A fully transparent cursor, for wl_pointer.set_cursor(null): the client
  // asked for the pointer to be invisible. Owned by the backend -- callers
  // never free it. 0 if the backend cannot make one.
  [[nodiscard]] virtual uint32_t blank_cursor() = 0;

  // Make `cursor` the pointer image while the pointer is over `window`.
  // cursor == 0 removes any cursor previously set on the window, restoring
  // the window system's default (on X11: inherit the parent's, ultimately the
  // root's arrow). Asynchronous and fire-and-forget like every other request
  // here; safe to call from inside live Wayland request dispatch.
  virtual void set_window_cursor(uint32_t window, uint32_t cursor) = 0;

  // wl_surface.set_input_region translated to X11's Shape extension
  // (XShapeCombineRectangles on the input shape): `add_rects` become the
  // window's clickable area (unioned together), then `sub_rects` are
  // punched out of it.
  //
  // `has_region` carries the bit `add_rects`/`sub_rects` alone can't: per
  // the Wayland spec, "no region was ever declared" (has_region == false,
  // the default — wl_surface.set_input_region(nullptr) or never called at
  // all) and "a region was declared and it happens to be empty"
  // (has_region == true, add_rects empty) are opposites, not the same
  // state. The former means accept input over the whole window; the
  // latter — the standard idiom for a purely-visual overlay/subsurface
  // that wants clicks and keys to fall through to whatever is beneath it
  // — means accept no input at all. A backend must be able to produce a
  // real, distinctly-empty shape for the latter case, not fall back to
  // the former. Backends without a native input-shape concept (or where
  // the Shape extension isn't available) may still implement this as a
  // no-op — surfaces just keep accepting input over their full rectangle
  // — which is a safe (if imprecise) fallback for the has_region == false
  // case, but note that it is *not* a safe fallback for has_region ==
  // true with empty rects, since that silently reverts the click/key
  // pass-through the client asked for.
  virtual void set_input_shape(uint32_t window, bool has_region,
                               const std::vector<InputRect> &add_rects,
                               const std::vector<InputRect> &sub_rects) = 0;

  // --- Presentation ----------------------------------------------------------
  // CPU-copy path: blit raw pixels into the window. `width`/`height` are the
  // buffer's full dimensions; `src` selects the sub-rectangle of it that is
  // the window (see BufferRegion), defaulting to the whole buffer.
  using FramePresentedCallback = std::function<void()>;
  virtual void present_pixels(uint32_t window, uint16_t width, uint16_t height,
                              uint32_t stride, const void *pixels,
                              BufferRegion src = {},
                              FramePresentedCallback on_presented = {}) = 0;

  // Zero-copy path: import a client dmabuf fd and present it directly.
  // Asynchronous: on X11 this is a request that *can* legitimately fail
  // on malformed client input (see X11Backend's implementation comment),
  // and confirming that without blocking needs a server round-trip that
  // must not stall the caller — the caller may be running from inside
  // live Wayland-request dispatch, and a window manager doing its own
  // synchronous X11 work at the same moment (a server grab during
  // reparenting, most commonly) can otherwise stall a blocking call
  // indefinitely, freezing the whole compositor for every client, not
  // just the one waiting on this import. `on_imported` fires exactly
  // once, with pixmap==0 on failure; it may fire after the call returns.
  // Backends without an import path may implement this as a no-op that
  // always calls back with 0 ("not presentable").
  // bgr_order: true for a buffer whose red/blue channels are swapped
  // relative to this window's visual (DRM_FORMAT_ABGR8888/XBGR8888) — the
  // backend must correct for that itself (e.g. via an extra compositing
  // step) rather than present the raw bytes, or colors come out wrong.
  using DmabufImportCallback = std::function<void(uint32_t pixmap)>;
  virtual void import_dmabuf(int fd, uint32_t window, uint16_t width,
                            uint16_t height, uint8_t depth, uint8_t bpp,
                            uint16_t stride, uint64_t modifier,
                            bool bgr_order,
                            DmabufImportCallback on_imported) = 0;
  // `src` selects the sub-rectangle of the pixmap that is the window (see
  // BufferRegion), defaulting to the whole pixmap.
  // on_source_consumed fires once the backend will no longer read from the
  // client-owned source pixmap (for X11, after an X-server fence following
  // the copy into compositor-owned retained storage). It is distinct from
  // on_presented: a buffer may be safe to reuse before the displayed frame
  // has completed presentation.
  virtual void present_pixmap(uint32_t window, uint32_t pixmap,
                              BufferRegion src = {}, int32_t buffer_scale = 1,
                              FramePresentedCallback on_presented = {},
                              FramePresentedCallback on_source_consumed = {},
                              uint16_t stretch_width = 0,
                              uint16_t stretch_height = 0) = 0;
  // Re-present the compositor-owned image scaled to an arbitrary logical
  // destination size. Used while a toplevel's WM target is ahead of its
  // latest client frame. Backends should use their native Render path for
  // pixmaps when possible and CPU scaling only for pixel-backed frames.
  virtual void repaint_window_scaled(uint32_t window, uint16_t width,
                                     uint16_t height,
                                     FramePresentedCallback on_presented = {}) = 0;
  // Re-present the compositor-owned image, optionally applying new surface
  // crop/scale state. buffer_scale <= 0 means preserve the cached scale.
  virtual void repaint_window(uint32_t window, BufferRegion src = {},
                              int32_t buffer_scale = 0,
                              FramePresentedCallback on_presented = {}) = 0;
  // Linear NV12 is a multi-plane YUV format rather than an X11 RGB pixmap.
  // Backends may convert it to their native presentation format; this
  // fallback is intentionally separate from zero-copy RGB dmabuf import.
  virtual void present_nv12(uint32_t window, uint16_t width, uint16_t height,
                            int fd_y, uint32_t offset_y, uint32_t stride_y,
                            int fd_uv, uint32_t offset_uv, uint32_t stride_uv,
                            BufferRegion src = {}, int32_t buffer_scale = 1,
                            FramePresentedCallback on_presented = {},
                            uint16_t stretch_width = 0,
                            uint16_t stretch_height = 0) = 0;

  virtual void free_pixmap(uint32_t pixmap) = 0;

  // --- Window-manager state -------------------------------------------------
  // Ask the window system to add or remove a state on a top-level window,
  // on behalf of xdg_toplevel.set_fullscreen / set_maximized. Asynchronous
  // by nature: the window system is free to refuse, and the size that
  // results (if any) arrives later through the ordinary resize callback,
  // not as a return value here.
  virtual void set_window_state(uint32_t window, WindowState state,
                                bool enabled) = 0;

  // xdg_toplevel.move / xdg_toplevel.resize: the client is asking for an
  // interactive, pointer-driven move or resize -- normally in response to
  // a button press on window decoration the client itself drew. As with
  // set_window_state above, WWindows never performs the move/resize
  // itself; it only ever forwards the request to whichever window system
  // mechanism hands control to the window manager, which is the one
  // actually dragging the window and is free to ignore the request
  // outright. The result (if any) arrives later through the ordinary
  // resize callback, same as set_window_state. Backends with no such
  // mechanism may implement both as a no-op.
  virtual void request_interactive_move(uint32_t window) = 0;
  virtual void request_interactive_resize(uint32_t window,
                                          ResizeEdge edge) = 0;

  // xdg_toplevel.set_minimized. Unlike set_fullscreen/set_maximized, this
  // is one-shot -- the protocol has no unset_minimized, because a client
  // cannot ask to be un-minimized; that only ever happens through the
  // window manager or the person operating it. Backends with no minimize
  // concept may implement this as a no-op.
  virtual void minimize_window(uint32_t window) = 0;

  // The render/scan-out device backing import_dmabuf(), identified the way
  // zwp_linux_dmabuf_feedback_v1.main_device wants it: the dev_t of its
  // device node (struct stat::st_rdev — i.e. what you'd get by fstat()-ing
  // the actual DRM device fd), packaged as raw bytes for the Wayland event.
  // std::nullopt means the backend currently has no such device (e.g. DRI3
  // never initialised), in which case register_linux_dmabuf() won't
  // advertise zwp_linux_dmabuf_v1 at all — a dmabuf global with no device
  // behind it can't import anything, so there is nothing useful to offer.
  [[nodiscard]] virtual std::optional<dev_t> dmabuf_main_device() const = 0;

  // --- Callbacks -------------------------------------------------------------
  using ResizeCallback =
      std::function<void(uint32_t window, uint16_t width, uint16_t height)>;
  virtual void set_resize_callback(ResizeCallback cb) = 0;

  // What the window manager itself says it has done to a toplevel, as
  // opposed to what the client asked for: the EWMH _NET_WM_STATE on X11.
  // Maximizing from the window manager's own title bar, keybinding or edge
  // snapping never passes through this compositor, so without this a client
  // would go on believing it is a free-floating window -- drawing its shadow
  // and snapping to whole character cells inside a window that is now
  // exactly as big as the screen work area.
  //
  // Horizontal and vertical maximization are reported separately because
  // half-tiled windows (snapped to a screen half) are typically maximized in
  // one axis only.
  struct WmWindowState {
    bool maximized_horz{false};
    bool maximized_vert{false};
    bool fullscreen{false};
    friend bool operator==(const WmWindowState &, const WmWindowState &) = default;
  };
  // Called each time the reported state of a toplevel may have changed
  // (callers must tolerate repeats). Never called for a backend, or a
  // window manager, that publishes nothing: the default is that this
  // is simply never set off.
  using WmStateCallback = std::function<void(uint32_t window, WmWindowState)>;
  virtual void set_wm_state_callback(WmStateCallback) {}

  // Invoked when the window system requests the window be closed.
  using CloseCallback = std::function<void(uint32_t window)>;
  virtual void set_close_callback(CloseCallback cb) = 0;

  // Raw input events, set by InputTranslator, cleared on its destruction.
  //   win  = backend-native window id the event arrived on
  //   time = backend-native timestamp in milliseconds
  struct InputCallbacks {
    // x,y are window-local (relative to `win`); root_x,root_y are relative
    // to the X11 root window. Local coordinates drive wl_pointer motion;
    // root coordinates are not used by the translator any more (the
    // pointer image is a window-system cursor, not an overlay that has to be
    // positioned by hand) but stay on the port for other consumers.
    std::function<void(uint32_t win, int16_t x, int16_t y, int16_t root_x,
                       int16_t root_y, uint32_t time)>
        on_motion;
    std::function<void(uint32_t win, uint8_t button, bool pressed,
                       uint32_t time)>
        on_button;
    std::function<void(uint32_t win, uint8_t keycode, bool pressed,
                       uint32_t time)>
        on_key;
    std::function<void(uint32_t win, int16_t x, int16_t y, int16_t root_x,
                       int16_t root_y)>
        on_enter;
    std::function<void(uint32_t win)> on_leave;
    std::function<void(uint32_t win)> on_expose;
    // Real X11 input focus arriving at / leaving a top-level window (a
    // window-manager decision — click-to-focus, alt-tab, a workspace
    // switch, a closed window's focus going somewhere else — this
    // backend has no say in and does not try to second-guess). Drives
    // wl_keyboard focus, kept deliberately independent of on_enter/
    // on_leave: those track the *pointer*, which is a different thing
    // Wayland clients expect to receive separately from keyboard focus,
    // and conflating the two (focus-follows-pointer, reimplemented
    // locally) stops working the moment the window manager's own focus
    // policy disagrees — e.g. any keyboard-driven window switch, which
    // never crosses a window boundary with the pointer at all.
    std::function<void(uint32_t win)> on_focus_in;
    std::function<void(uint32_t win)> on_focus_out;
    // The key transition that caused an XKB StateNotify. A corresponding core
    // key event may have been swallowed by a passive keyboard grab; the input
    // translator uses this to reconcile a release already owed to a Wayland
    // client. For ordinary key events this is harmless because the logical
    // pressed-key set will already reflect the transition.
    std::function<void(uint8_t keycode, bool pressed, uint32_t time)>
        on_xkb_key_transition;
    // A genuine XKB StateNotify from the server: the modifier/group state
    // changed for the keyboard device, regardless of which window currently
    // has focus or which client owns a grab. In particular it fires for a
    // modifier release consumed by a window-manager grab. depressed/latched/
    // locked map to xkb_state_update_mask's modifier arguments; group is the
    // server's effective layout group.
    std::function<void(uint32_t depressed, uint32_t latched, uint32_t locked,
                       uint32_t group)>
        on_xkb_state_changed;
    // The server's keymap itself changed — XkbMapNotify (a live
    // setxkbmap, or WWindows simply starting before whatever normally
    // configures the session's real layout got to run: an i3
    // exec/exec_always line, ~/.xprofile, xorg.conf's XkbLayout) or
    // XkbNewKeyboardNotify (the core keyboard device itself was swapped).
    // No payload: the handler is expected to just call
    // keyboard_keymap_string() again and recompile from scratch, exactly
    // like the very first fetch at startup — trying to interpret the
    // event's own delta fields buys nothing a full re-fetch doesn't
    // already give for free, and this fires rarely enough that the cost
    // of always doing the full round-trip doesn't matter.
    std::function<void()> on_xkb_keymap_changed;
    // The server's keyboard auto-repeat setting changed (XkbControlsNotify:
    // `xset r rate`, a desktop environment's keyboard preferences, or repeat
    // being switched off). No payload, like on_xkb_keymap_changed: the handler
    // calls keyboard_repeat() again.
    std::function<void()> on_xkb_repeat_changed;
  };
  virtual void set_input_callbacks(InputCallbacks cbs) = 0;

  // The X server's actual keymap for the core keyboard device, serialized
  // (XKB_KEYMAP_FORMAT_TEXT_V1) so the caller can compile its own
  // independent xkb_keymap/xkb_state from it without this port leaking
  // any X11-specific type. Returns nullopt if the server's XKB extension
  // couldn't be used (very rare in practice — XKB has been effectively
  // mandatory since the mid-2000s — but not assumed).
  //
  // This is not optional polish: a compositor that instead fabricates a
  // keymap from bare defaults (empty rules/model/layout/variant/options)
  // silently disagrees with whatever the person's actual session is
  // configured to use — a non-default layout, or any of the extremely
  // common personal `setxkbmap -option` customizations (swapping Caps
  // Lock and Control being the most popular) — for every key, not just
  // modifiers, for as long as the compositor runs.
  [[nodiscard]] virtual std::optional<std::string> keyboard_keymap_string() = 0;

  // Keyboard auto-repeat as the window system has it configured -- what
  // `xset r rate` and a desktop environment's keyboard settings change.
  // Wayland has the *client* generate repeats and the compositor merely
  // announce the rate (wl_keyboard.repeat_info), so what is announced has to
  // be the user's setting, not a number this compositor made up.
  struct KeyRepeat {
    uint32_t rate_hz{0};   // 0 = auto-repeat is switched off
    uint32_t delay_ms{0};  // before the first repeat
    friend bool operator==(const KeyRepeat &, const KeyRepeat &) = default;
  };
  // nullopt if the window system can't say (a backend with no real session to
  // ask, or the XKB extension unavailable); the caller then keeps a default.
  [[nodiscard]] virtual std::optional<KeyRepeat> keyboard_repeat() {
    return std::nullopt;
  }

  // The X11 keycodes (in the same numbering on_key's callback uses) the
  // window system currently reports as physically held down, queried
  // fresh from the server rather than reconstructed from event history.
  //
  // Exists to resynchronize keyboard modifier state when some of a key
  // combination's events were delivered somewhere this backend never
  // sees. The common case: a window manager's own key binding. Firing a
  // bound combination actively grabs the *entire* keyboard for as long
  // as any of its keys stay down (see XGrabKey), which redirects
  // everything for the rest of that key-down — typically including the
  // modifiers' own release — to the grabbing client instead of whichever
  // window has real focus. A compositor that only ever tracks state
  // incrementally from events it personally received is left believing
  // a modifier is still held long after the physical key came back up.
  // Called whenever real keyboard focus changes (see on_focus_in above),
  // so it self-heals regardless of how many events were missed or why.
  [[nodiscard]] virtual std::vector<uint8_t> query_pressed_keycodes() = 0;

  // Invoked after the backend has processed one batch of incoming events.
  virtual void set_events_processed_callback(std::function<void()> cb) = 0;

  // --- Clipboard (wl_data_device selection <-> X11 CLIPBOARD) --------------
  // Deliberately scoped to text-ish mime types only — see X11Backend's
  // implementation for the exact atom set. Binary/image clipboard content
  // still works between two WWindows-managed Wayland clients (that path
  // never touches the backend at all), it just isn't mirrored to X11.
  //
  // Take (or release, with an empty list) ownership of the X11 CLIPBOARD
  // selection on behalf of the Wayland client that currently holds it, so
  // native X11 applications can paste it. `mime_types` is whatever the
  // owning wl_data_source advertised; backends that can't map any of them
  // to something X11 understands may just treat this as a no-op.
  virtual void set_clipboard_owner(std::vector<std::string> mime_types) = 0;

  // Called back when a native X11 application asks for the clipboard data
  // WWindows is currently advertising (i.e. while set_clipboard_owner() is
  // in effect) in a specific mime type. The callee must eventually write
  // the data to `fd` and close it (mirrors wl_data_source.send) — never
  // called for a mime type that wasn't in the last set_clipboard_owner()
  // list.
  using ClipboardSendCallback =
      std::function<void(const std::string &mime_type, int fd)>;
  virtual void set_clipboard_send_callback(ClipboardSendCallback cb) = 0;

  // Called back whenever the X11 CLIPBOARD selection is owned by something
  // other than us — a native X11 application, or nobody. `mime_types` is
  // the (possibly empty) set of mime types WWindows could map from that
  // owner's advertised targets; empty means "no usable selection".
  using ClipboardOwnerChangedCallback =
      std::function<void(std::vector<std::string> mime_types)>;
  virtual void
  set_clipboard_owner_changed_callback(ClipboardOwnerChangedCallback cb) = 0;

  // Asynchronously fetch the CLIPBOARD contents currently owned by a
  // native X11 application, in the given mime type (one previously
  // reported via ClipboardOwnerChangedCallback). `on_data` fires exactly
  // once, with an empty vector on failure/timeout/unavailable type.
  using ClipboardDataCallback = std::function<void(std::vector<uint8_t> data)>;
  virtual void request_clipboard_data(const std::string &mime_type,
                                      ClipboardDataCallback on_data) = 0;
};

} // namespace ww
