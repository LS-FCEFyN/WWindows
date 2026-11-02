#pragma once

#include <cstdint>
#include <functional>
#include <memory>

struct wl_resource;
struct wl_display;

namespace ww {

class IWindowBackend;
class ISurfaceInteractions;

// Bridges raw X11 input events to Wayland wl_pointer / wl_keyboard events.
// Owns the xkb context, keymap, and live modifier state.
//
// Lifetime contract (enforced by WWindows::Impl member order):
//   InputTranslator is destroyed BEFORE the window backend and WaylandServer so that
//   X11 callbacks are cleared and Wayland resources are already gone.
class InputTranslator {
public:
  InputTranslator(IWindowBackend &backend, ISurfaceInteractions &surfaces);
  ~InputTranslator();

  InputTranslator(const InputTranslator &) = delete;
  InputTranslator &operator=(const InputTranslator &) = delete;

  // Must be called once after WaylandServer::create() gives us a wl_display.
  // Initialises xkb and builds the keymap string sent to keyboard clients.
  void set_display(wl_display *display);

  // Called by CoreGlobals when a client binds wl_pointer / wl_keyboard.
  // The corresponding remove_* is called from the resource destructor callback.
  void add_pointer(wl_resource *resource);
  void remove_pointer(wl_resource *resource);
  void add_keyboard(wl_resource *resource);
  void remove_keyboard(wl_resource *resource);

  // Called by CoreGlobals on wl_pointer.set_cursor. `pointer_resource` is
  // the wl_pointer the request arrived on (used to check it belongs to the
  // client that currently has pointer focus); `surface_resource` is the
  // cursor image, or null to hide the cursor.
  void set_cursor(wl_resource *pointer_resource, wl_resource *surface_resource,
                  int32_t hotspot_x, int32_t hotspot_y);

  // --- wl_data_device drag-and-drop -----------------------------------------
  // While a drag is active, ordinary wl_pointer dispatch (enter/leave/
  // motion/button) is suppressed and these fire instead. No explicit X11
  // pointer grab is used: X11 already delivers per-window Enter/Leave/
  // Motion for whichever window is actually under the pointer even while a
  // button is held (nothing else has grabbed it), so that ordinary event
  // stream does exactly the hit-testing a drag needs — see
  // InputTranslator_pointer.cpp.
  struct DragCallbacks {
    // Pointer entered a new target surface — including the very first
    // target, synthesized immediately from whatever the pointer already
    // sat over when the drag began. x,y are surface-local.
    std::function<void(wl_resource *surface, int16_t x, int16_t y)> on_enter;
    // Pointer moved within the current target surface.
    std::function<void(int16_t x, int16_t y, uint32_t time)> on_motion;
    // Pointer left the current target surface.
    std::function<void()> on_leave;
    // The button held for the drag was released — always ends the drag
    // (as a drop if there's a current target, a cancel otherwise). The
    // callback is responsible for calling end_drag().
    std::function<void()> on_release;
  };

  // Starts redirecting pointer dispatch into `cbs`, and immediately
  // synthesizes an initial on_enter if the pointer already sits over a
  // known surface (it normally does — the drag started from a button
  // press there). Called from wl_data_device.start_drag.
  void begin_drag(DragCallbacks cbs);
  // Stops the redirect and resumes ordinary wl_pointer dispatch. Safe to
  // call even when no drag is active.
  void end_drag();

  // Fires whenever keyboard focus changes (including to null, when it's
  // cleared). Used by DataDeviceManager to know which client's
  // wl_data_device should (re)receive the current clipboard selection.
  using KeyboardFocusChangedCallback = std::function<void(wl_resource *)>;
  void set_keyboard_focus_changed_callback(KeyboardFocusChangedCallback cb);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ww