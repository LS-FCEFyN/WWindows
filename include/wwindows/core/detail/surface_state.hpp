#pragma once

#include "wwindows/core/policy/resize_policy.hpp"
#include "wwindows/ports/imported_buffer.hpp"
#include "wwindows/ports/resize_timer.hpp"
#include "wwindows/ports/window_backend.hpp"
#include "wwindows/ports/window_manager_watcher.hpp"
#include <wayland-server-core.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>
#include <memory>
#include <deque>
#include <optional>
#include <variant>

struct wl_resource;

namespace ww {

// Short alias used throughout the surface layer.
namespace policy = ::ww::core::policy;

struct FrameCallbackState {
  wl_resource *resource{nullptr};
  wl_listener destroy_listener{};
  std::weak_ptr<bool> surface_alive;

  FrameCallbackState() { wl_list_init(&destroy_listener.link); }

  static void on_destroy(wl_listener *listener, void *) {
    FrameCallbackState *callback =
        wl_container_of(listener, callback, destroy_listener);
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
    callback->resource = nullptr;
  }

  ~FrameCallbackState() {
    // A callback state can be the last owner after an asynchronous backend
    // operation is cancelled/dropped. In that case no presentation callback
    // remains to destroy the Wayland resource. Always destroy an undelivered
    // resource here; the resource destroy listener nulls it first on normal
    // completion or client-initiated destruction.
    if (resource)
      wl_resource_destroy(resource);
    if (!wl_list_empty(&destroy_listener.link)) {
      wl_list_remove(&destroy_listener.link);
      wl_list_init(&destroy_listener.link);
    }
  }
};

// An RGB dmabuf import that is still in flight. Newer commits of that same
// buffer update this mutable request, so the import callback resolves the
// latest commit rather than presenting an obsolete one.
struct ImportInFlight {
  wl_resource *buffer{nullptr};
  uint64_t generation{0};
  BufferRegion region{};
  int32_t buffer_scale{1};
  uint16_t stretch_width{0};
  uint16_t stretch_height{0};
  std::function<void()> on_presented;
  std::function<void()> on_source_consumed;
};

// Shared with asynchronous completions (as a weak_ptr) so one that outlives
// its surface, or its commit, can tell and do nothing.
struct SurfacePresentationLifetime {
  bool alive{true};
  // The one counter kept for async work: every applied commit bumps it, and a
  // completion carrying an older generation is dropped.
  uint64_t generation{0};
};

// Everything about a surface's asynchronous presentation: the generation
// counter and the import currently in flight, if any.
struct AsyncPresentation {
  std::shared_ptr<SurfacePresentationLifetime> lifetime{
      std::make_shared<SurfacePresentationLifetime>()};
  std::shared_ptr<ImportInFlight> importing;

  // Abandons the in-flight import. The backend callback may still arrive, but
  // its generation/state checks keep it from presenting; dropping
  // on_presented releases any frame callbacks the import was holding.
  void cancel_import() {
    if (importing)
      importing->on_presented = {};
    importing.reset();
  }
};

// --- the content last attached to a surface --------------------------------
// The kind of storage that backs the surface's current content is one of
// these, never a bool beside unrelated fields:
struct NoContent {};
// wl_shm: a compositor-owned, tightly packed copy so a geometry or scale
// change never re-reads a released wl_buffer. `pixels` is empty between the
// buffer being retired (size known) and the first present (pixels copied), so
// a stale copy of an *earlier* frame can never pair with a new size.
struct ShmContent {
  uint32_t stride{0};
  std::vector<uint8_t> pixels;
};
// A dmabuf-backed (or other non-SHM) buffer: nothing is retained here; the
// backend keeps its own source until Present IDLE_NOTIFY.
struct DmabufContent {};

struct BufferContent {
  uint16_t width{0};
  uint16_t height{0};
  std::variant<NoContent, ShmContent, DmabufContent> storage;

  [[nodiscard]] bool has_size() const { return width > 0 && height > 0; }
  // The retained SHM copy, or nullptr when there is none to replay.
  [[nodiscard]] const ShmContent *retained_shm() const {
    const auto *shm = std::get_if<ShmContent>(&storage);
    return shm && !shm->pixels.empty() ? shm : nullptr;
  }
  void clear() { *this = BufferContent{}; }
};

// --- double-buffered wl_surface / xdg requests ----------------------------
// Everything a client can request that only takes effect at the next commit
// lives in PendingState. An engaged optional means "requested since the last
// commit"; nullopt means "not requested", so a request and its absence are
// never encoded as a flag beside a value. commit() applies and resets each.

// wl_surface.attach is three different requests once it reaches commit():
struct PendingAttachBuffer { wl_resource *buffer{nullptr}; };
// wl_surface.attach(NULL): unmap / drop the current content.
struct PendingDetach {};
// A buffer was attached and then destroyed before commit. This is NOT an
// attach(NULL): the committed image stays, only the stale request is dropped.
struct PendingAttachInvalidated {};
using PendingAttach =
    std::variant<PendingAttachBuffer, PendingDetach, PendingAttachInvalidated>;

// xdg_surface.set_window_geometry: which part of the buffer is the window.
struct WindowGeometry {
  int32_t x{0}, y{0}, w{0}, h{0};
};

// wl_surface.set_input_region. has_region == false is the NULL region (whole
// surface); has_region with empty rect lists is an explicitly empty region.
struct PendingInputRegion {
  bool has_region{false};
  std::vector<InputRect> add;
  std::vector<InputRect> sub;
};

struct PendingState {
  std::optional<PendingAttach> attach;
  // Outer: set_window_geometry was called since the last commit. Inner:
  // nullopt means the client cleared it (zero/negative size == unset).
  std::optional<std::optional<WindowGeometry>> window_geometry;
  // The complete limits that will be active after commit. Seeded from the
  // committed limits by the first set_min_size/set_max_size so that setting
  // one bound never resets the other.
  std::optional<WindowSizeLimits> size_limits;
  std::optional<PendingInputRegion> input_region;
  std::optional<int32_t> buffer_scale;
  // wl_surface.offset (and attach's deprecated x,y): reset by every commit.
  int32_t offset_x{0};
  int32_t offset_y{0};
  // damage / damage_buffer: the backend repaints the full buffer, so a bool
  // is sufficient.
  bool damaged{false};

  // wp_viewport: source rectangle (crop) in fixed-point surface-local
  // coordinates (after buffer_transform and buffer_scale). When all four
  // are -1.0 (wl_fixed_from_int(-1)), the source is unset.
  wl_fixed_t viewport_src_x{0};
  wl_fixed_t viewport_src_y{0};
  wl_fixed_t viewport_src_width{0};
  wl_fixed_t viewport_src_height{0};
  bool viewport_src_unset{true};

  // wp_viewport: destination size (scale) in surface-local coordinates.
  // When both are -1, the destination is unset.
  int32_t viewport_dst_width{-1};
  int32_t viewport_dst_height{-1};
  bool viewport_dst_unset{true};

  [[nodiscard]] wl_resource *attached_buffer() const {
    if (!attach)
      return nullptr;
    const auto *b = std::get_if<PendingAttachBuffer>(&*attach);
    return b ? b->buffer : nullptr;
  }
  [[nodiscard]] bool detach_requested() const {
    return attach && std::holds_alternative<PendingDetach>(*attach);
  }
};

// --- what the last commit left in force ------------------------------------
// The counterpart of PendingState: values a client set once and expects to
// keep applying to every later commit. commit() is the only writer.
struct Committed {
  // The window geometry in force. A client that draws its own shadow and
  // invisible resize border (GTK, Firefox, Qt CSD) attaches a buffer larger
  // than the configured size and uses this to say where the real window sits
  // inside it.
  std::optional<WindowGeometry> geometry;

  // Cumulative wl_surface.offset: where the buffer's upper-left corner sits
  // relative to the previous one, in surface-local coordinates. Applied as a
  // correction when translating geometry / input regions into buffer pixels
  // (see buffer_region()), never as a window move, and deliberately not
  // applied to subsurface positions.
  int32_t offset_x{0};
  int32_t offset_y{0};

  // wl_surface.set_buffer_scale. The X11 output is 1:1 logical pixels, so
  // presentation downsamples by this integer factor.
  int32_t buffer_scale{1};

  // xdg_toplevel min/max size; zero in a dimension is "no limit".
  WindowSizeLimits size_limits;

  // Logical size of the last client buffer committed (what the client drew,
  // after window-geometry crop and buffer_scale; comparable with configure
  // dimensions). Not whether the backend presented it. nullopt: nothing
  // committed, or the commit had no usable size.
  std::optional<policy::WindowSize> content_size;

  // wp_viewport: committed source rectangle (crop). When unset, the whole
  // buffer (after window-geometry crop and buffer_scale) is used.
  wl_fixed_t viewport_src_x{0};
  wl_fixed_t viewport_src_y{0};
  wl_fixed_t viewport_src_width{0};
  wl_fixed_t viewport_src_height{0};
  bool viewport_src_unset{true};

  // wp_viewport: committed destination size (scale). When unset, the surface
  // size is determined by the buffer and window-geometry.
  int32_t viewport_dst_width{-1};
  int32_t viewport_dst_height{-1};
  bool viewport_dst_unset{true};

  // The geometry rect clamped to a concrete buffer size, in buffer pixels
  // (geometry.x/y are surface-local, offset moves the buffer origin, scale
  // converts to buffer pixels). Empty when no usable geometry is declared.
  [[nodiscard]] BufferRegion buffer_region(int32_t buffer_w,
                                           int32_t buffer_h) const {
    if (!geometry || geometry->w <= 0 || geometry->h <= 0)
      return {};
    const int32_t scale = std::max<int32_t>(buffer_scale, 1);
    const int64_t scaled_x =
        (static_cast<int64_t>(geometry->x) - offset_x) * scale;
    const int64_t scaled_y =
        (static_cast<int64_t>(geometry->y) - offset_y) * scale;
    const int64_t scaled_w = static_cast<int64_t>(geometry->w) * scale;
    const int64_t scaled_h = static_cast<int64_t>(geometry->h) * scale;
    const int32_t buf_x = static_cast<int32_t>(std::clamp<int64_t>(
        scaled_x, INT32_MIN, INT32_MAX));
    const int32_t buf_y = static_cast<int32_t>(std::clamp<int64_t>(
        scaled_y, INT32_MIN, INT32_MAX));
    const int32_t x = std::clamp(buf_x, 0, buffer_w);
    const int32_t y = std::clamp(buf_y, 0, buffer_h);
    const int32_t w = static_cast<int32_t>(std::min<int64_t>(
        scaled_w, static_cast<int64_t>(buffer_w) - x));
    const int32_t h = static_cast<int32_t>(std::min<int64_t>(
        scaled_h, static_cast<int64_t>(buffer_h) - y));
    if (w <= 0 || h <= 0)
      return {};
    return BufferRegion{static_cast<int16_t>(x), static_cast<int16_t>(y),
                        static_cast<uint16_t>(w), static_cast<uint16_t>(h)};
  }
};

// --- surface roles ----------------------------------------------------------
// A wl_surface holds exactly one role, so the role is one variant and each
// alternative owns only the state that role needs. State that belongs to no
// single role (the X11 window handle, pending/committed requests, content,
// frame callbacks, destroy listeners) stays on Surface.
//
// Destroy listeners deliberately stay on Surface rather than inside an
// alternative: wl_listener links are address-based, and a variant alternative
// is destroyed and rebuilt when the role changes.

// No role yet. A commit that arrives now is parked (Surface::commit_parked).
struct NoRole {};

// Where a child X11 window sits inside its parent's window. A popup is
// structurally a subsurface-like child window whose position comes from an
// xdg_positioner instead of wl_subsurface.set_position.
struct ChildPlacement {
  wl_resource *parent{nullptr}; // owning parent wl_surface
  int32_t x{0};
  int32_t y{0};
};

struct SubsurfacePosition {
  int32_t x{0};
  int32_t y{0};
  bool operator==(const SubsurfacePosition &) const = default;
};

// wl_subsurface.place_above/place_below, deferred to the parent's next apply.
// Only the most recently requested restack survives: indistinguishable in
// practice for any client that does not alternate place_above/place_below
// against different siblings between the same two parent applies.
struct PendingRestack {
  wl_resource *sibling{nullptr}; // nulled if that sibling is destroyed first
  bool above{true};
};

struct Subsurface {
  ChildPlacement placement;
  // wl_subsurface's own sync mode, initially synchronized per spec. Combines
  // with ancestors: SurfaceManager::Impl::is_effectively_synchronized() is the
  // rule (synchronized if its own mode says so, or any ancestor's does).
  bool sync{true};
  // Whether a synchronized commit has cached this subsurface's wl_surface
  // state (the usual PendingState) since the parent last applied it.
  bool has_cached_state{false};
  // wl_subsurface.set_position, always double-buffered against the *parent's*
  // state being applied regardless of this subsurface's own sync mode.
  // `placement.x/y` are the active values; cascade_to_children() copies this
  // into them.
  std::optional<SubsurfacePosition> pending_position;
  std::optional<PendingRestack> pending_restack;
};

// xdg_popup. Participates in the click-outside dismiss stack
// (SurfaceManager::Impl::popup_stack) instead of place_above/place_below
// stacking.
struct Popup {
  ChildPlacement placement;
  wl_resource *xdg_popup{nullptr}; // for popup_done
};

// wl_pointer.set_cursor. A cursor surface has no X11 window at all: each
// commit flattens its buffer into a window-system cursor object, owned here
// -- replaced on the next commit, freed when the role is cleared or the
// surface dies.
struct Cursor {
  uint32_t handle{0};
  // Surface-local, as set_cursor gave it; wl_surface.offset moves it along
  // with the content (see apply_cursor_surface_state).
  int32_t hotspot_x{0};
  int32_t hotspot_y{0};
  std::function<void()> changed;
};

// The latest size the WM asked for, and what has happened to it since.
// ConfigureNotify doesn't say who caused a resize, so a new target is
// recognised by differing from the previous one; a new target starts a new
// generation and clears `adopted`/`overridden`.
struct WmTarget {
  policy::WindowSize size;
  // The adopt settle window is measured from when the WM first reported it.
  IResizeTimer::TimePoint since{};
  uint64_t generation{1};
  // false: provisional seed from set_toplevel() (the 800x600 window we
  // created), not something the WM has said. The settle window and adoption
  // only begin once the WM has actually reported a target.
  bool observed{false};
  // adopt_committed_geometry() resized X11 away from this target.
  bool adopted{false};
  // The WM put the window back to this target right after we adopted a
  // smaller committed size, i.e. it enforces that size (tiling). Adopting
  // again would start an adopt -> re-tile loop, so stop for this target.
  bool overridden{false};

  // The WM target stood unchanged but the settle interval must start over
  // (e.g. the window returns to floating after the WM owned its geometry).
  void restart(IResizeTimer::TimePoint now) {
    since = now;
    ++generation;
    adopted = false;
  }
};

struct ExpectedResizeEcho {
  uint16_t width{0};
  uint16_t height{0};
  IResizeTimer::TimePoint expires_at{};
};

// xdg_surface.configure bookkeeping. xdg_surface is shared by toplevels and
// popups and exists before any role is assigned, so this is not role state:
// both roles record the serials they send, and ack_configure is validated
// against them whichever role the surface holds.
struct ConfigureLog {
  // Bounded history of serials sent but not yet acknowledged, for validation.
  std::deque<uint32_t> sent;
  // Most recently acknowledged serial; nullopt until the client acks one.
  std::optional<uint32_t> last_acked;
};

// xdg_toplevel + its xdg_surface.
struct Toplevel {
  wl_resource *toplevel{nullptr};    // nulled when the resource is destroyed
  wl_resource *xdg_surface{nullptr}; // likewise
  // Both resources still exist, i.e. configures can be sent.
  [[nodiscard]] bool live() const { return toplevel && xdg_surface; }

  // --- size: four facts, four owners (core/policy/resize_policy.hpp) ------
  // Each is an optional so "not known yet" is a state of the type rather than
  // a bool beside a pair of zeroes.
  //
  // The last xdg_toplevel.configure sent. Kept so an identical one isn't sent
  // again: a window manager routinely emits several ConfigureNotify events for
  // one logical decision, and each would otherwise cost the client a redraw
  // and a serial. States are part of the comparison: maximized -> tiled at an
  // unchanged size must still reach the client.
  std::optional<policy::ConfiguredState> configured;
  // The X11 window's size as WWindows last observed or requested it. Never
  // overwritten by a client commit alone: adoption writes it only when it
  // actually asks X to resize.
  std::optional<policy::WindowSize> x11_size;
  std::optional<WmTarget> wm_target;
  // The committed size we last re-sent the tile size against (see
  // policy::should_reassert_configure); cleared when the client obeys.
  std::optional<policy::WindowSize> reasserted_against;
  // Resizes WWindows itself asked X for, awaiting their ConfigureNotify. A
  // ConfigureNotify doesn't identify its cause, and echoes can arrive after a
  // newer request, so delayed/duplicate echoes are matched against this short-
  // lived list instead of being misread as fresh WM targets.
  std::vector<ExpectedResizeEcho> expected_resize_echoes;
  // While a WM resize is unresolved, old retained content is stretched to the
  // WM target. Floating windows may adopt the most recent committed geometry
  // only after all configures are acknowledged and the target has been stable
  // for 250 ms. The injected timer keeps this deterministic in tests.
  bool waiting_for_resize_frame{false};
  IResizeTimer::TimerId resize_settle_timer{0};

  // Where stale content should be stretched to: the WM's target if one was
  // observed, else whatever the client was last configured to.
  [[nodiscard]] policy::WindowSize resize_target() const {
    if (wm_target)
      return wm_target->size;
    return configured ? configured->size : policy::WindowSize{};
  }

  // --- what the client asked for vs. what the window manager has done -----
  // The window manager owns fullscreen and maximized. The two flags below are
  // only the client's latest *request*, forwarded to the WM and awaiting its
  // answer; they are never reported back to the client as a state. They keep
  // the resize policy from adopting a client size while the WM may be about to
  // take the geometry over, and are dropped as soon as the WM is seen undoing
  // the state (so the client's next request is not swallowed as a repeat).
  bool wants_fullscreen{false};
  bool wants_maximized{false};

  // What the WM actually reports (EWMH _NET_WM_STATE), verbatim. This -- not
  // the request above -- is what the client is told in its configure states:
  // a WM may refuse, or may fullscreen/maximize a window the client never
  // asked about (i3's own `fullscreen toggle`), and the client has to learn
  // the truth either way.
  IWindowBackend::WmWindowState wm;

  // Per-edge tiling as the WM's own IPC reports it (see IWindowManagerWatcher;
  // X itself never says). Kept apart from `wm` so neither source can overwrite
  // the other; effective_tiled() combines them.
  TiledEdges tiled;

  [[nodiscard]] bool wm_maximized_both_axes() const {
    return wm.maximized_horz && wm.maximized_vert;
  }
  // The edges the client is told are tiled. A window the WM has fullscreened
  // is by definition not part of the tiling; one it has maximized on both
  // axes fills exactly its area and is indistinguishable from tiled to a
  // client, so it is reported as tiled on every edge.
  [[nodiscard]] TiledEdges effective_tiled() const {
    if (wm.fullscreen)
      return {};
    if (wm_maximized_both_axes())
      return TiledEdges{true, true, true, true};
    return tiled;
  }
  // This toplevel holds keyboard focus. Observed from the input translator
  // (itself driven by X FocusIn/FocusOut), never decided here. Reported to the
  // client as the xdg_toplevel ACTIVATED state, which is what clients key
  // "my window looks focused" on -- foot, for one, only blinks its cursor,
  // draws a filled rather than hollow one, and highlights its title/borders
  // while activated.
  bool activated{false};
};

using Role = std::variant<NoRole, Toplevel, Popup, Subsurface, Cursor>;

struct Surface {
  Surface() = default;
  // Pinned in place: the wl_listeners below are linked into libwayland's
  // intrusive lists by address, and SurfaceManager hands out wl_container_of
  // pointers to this object. Copying or moving it would leave those links
  // pointing at the old location, so neither is allowed; it is only ever
  // constructed in place inside Impl::surfaces (a node-based map).
  Surface(const Surface &) = delete;
  Surface &operator=(const Surface &) = delete;

  // The backend window this surface is shown in; 0 for a surface that has
  // none (no role yet, a cursor, or a subsurface not yet realized).
  uint32_t x11_window{0};
  Role role;

  [[nodiscard]] Toplevel *as_toplevel() { return std::get_if<Toplevel>(&role); }
  [[nodiscard]] const Toplevel *as_toplevel() const { return std::get_if<Toplevel>(&role); }
  [[nodiscard]] Popup *as_popup() { return std::get_if<Popup>(&role); }
  [[nodiscard]] const Popup *as_popup() const { return std::get_if<Popup>(&role); }
  [[nodiscard]] Subsurface *as_subsurface() { return std::get_if<Subsurface>(&role); }
  [[nodiscard]] const Subsurface *as_subsurface() const { return std::get_if<Subsurface>(&role); }
  [[nodiscard]] Cursor *as_cursor() { return std::get_if<Cursor>(&role); }
  [[nodiscard]] const Cursor *as_cursor() const { return std::get_if<Cursor>(&role); }

  // A toplevel whose xdg_toplevel and xdg_surface both still exist.
  [[nodiscard]] Toplevel *live_toplevel() {
    auto *t = as_toplevel();
    return t && t->live() ? t : nullptr;
  }
  [[nodiscard]] const Toplevel *live_toplevel() const {
    const auto *t = as_toplevel();
    return t && t->live() ? t : nullptr;
  }
  // Resize bookkeeping only exists for a toplevel; every other surface is
  // never "waiting for a resize frame" and has no resize target.
  [[nodiscard]] bool waiting_for_resize_frame() const {
    const auto *t = as_toplevel();
    return t && t->waiting_for_resize_frame;
  }
  void stop_waiting_for_resize_frame() {
    if (auto *t = as_toplevel())
      t->waiting_for_resize_frame = false;
  }
  [[nodiscard]] policy::WindowSize resize_target() const {
    const auto *t = as_toplevel();
    return t ? t->resize_target() : policy::WindowSize{};
  }
  // The xdg_toplevel resource still exists (the xdg_surface may not).
  [[nodiscard]] bool has_toplevel() const {
    const auto *t = as_toplevel();
    return t && t->toplevel;
  }
  [[nodiscard]] bool has_role() const { return !std::holds_alternative<NoRole>(role); }
  [[nodiscard]] bool is_subsurface() const { return std::holds_alternative<Subsurface>(role); }
  [[nodiscard]] bool is_popup() const { return std::holds_alternative<Popup>(role); }
  [[nodiscard]] bool is_cursor() const { return std::holds_alternative<Cursor>(role); }

  // Subsurfaces and popups are both child windows of a parent surface.
  [[nodiscard]] ChildPlacement *placement() {
    if (auto *s = as_subsurface())
      return &s->placement;
    if (auto *p = as_popup())
      return &p->placement;
    return nullptr;
  }
  [[nodiscard]] const ChildPlacement *placement() const {
    return const_cast<Surface *>(this)->placement();
  }
  [[nodiscard]] wl_resource *parent_surface() const {
    const auto *c = placement();
    return c ? c->parent : nullptr;
  }

  ConfigureLog configure_log;

  PendingState pending;
  Committed committed;
  // What the last attached wl_buffer put on this surface; see BufferContent.
  BufferContent content;
  wl_resource *last_buffer{nullptr};
  AsyncPresentation presentation;

  std::vector<std::shared_ptr<FrameCallbackState>> frame_callbacks;
  std::shared_ptr<bool> frame_callbacks_alive{std::make_shared<bool>(true)};
  std::vector<std::weak_ptr<FrameCallbackState>> all_frame_callbacks;

  // Latch for a one-time diagnostic about an unsupported buffer scale.
  bool warned_about_buffer_scale{false};

  // xdg_wm_base.get_xdg_surface: whether this wl_surface already has an
  // xdg_surface created for it. It exists before any role is assigned, so it
  // cannot live in Toplevel. See SurfaceManager::has_xdg_surface_role's doc
  // comment for what it guards against.
  bool has_xdg_surface_role{false};

  // Whether WWindows has asked X to map this window. This is a record of our
  // own request (a lifecycle fact: "the first eligible content has been
  // committed"), deliberately NOT a mirror of the X server's mapped state --
  // the WM may unmap a window on its own (e.g. a hidden workspace) and that
  // must not make the next commit re-map it against the WM's wishes. The
  // backend does not report Map/UnmapNotify to the core for the same reason.
  //
  // A toplevel is not mapped at set_toplevel() time: a client sends set_title
  // / set_app_id / set_parent / set_min_size after get_toplevel and before
  // its first commit, and a window manager decides where a window goes
  // (workspace, floating rules) when it first sees it mapped, so the window
  // must carry all of that by then. Mapping waits for the first commit, the
  // client's own "the role is fully described" signal.
  bool x11_map_requested{false};

  // A commit arrived while this surface had no X11 window, so
  // apply_surface_state had nowhere to put it. Wayland clients are free to
  // attach+commit a would-be cursor surface *before* calling
  // wl_pointer.set_cursor with it; the pending buffer is left in place and
  // applied once the role arrives.
  bool commit_parked{false};

  // Child surfaces (subsurfaces and popups) of this surface, whatever this
  // surface's own role is.
  std::vector<wl_resource *> subsurface_children;

  wl_listener destroy_listener{};             // guards pending.attach's buffer
  wl_listener last_buffer_destroy_listener{}; // guards last_buffer
  wl_listener toplevel_destroy_listener{};    // guards Toplevel::toplevel
  wl_listener xdg_surface_destroy_listener{}; // guards Toplevel::xdg_surface
};
} // namespace ww
