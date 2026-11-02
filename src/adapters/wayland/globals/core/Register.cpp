// register_core_globals() — the public entry point declared in
// CoreGlobals.hpp. Its only job is standing up the four globals
// implemented in Compositor.cpp, Seat.cpp, Output.cpp and
// Subcompositor.cpp; see impl.hpp for how those files fit together.
#include "wwindows/adapters/wayland/globals/core/detail/impl.hpp"
#include "wwindows/input/input_translator.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wwindows/support/log.hpp>

namespace ww {

Result<OutputGlobal *> register_core_globals(wl_display *display,
                                             SurfaceManager &surface_manager,
                                             InputTranslator &input_translator,
                                             const OutputState &output_state) {
  if (!wl_global_create(display, &wl_compositor_interface, 5, &surface_manager,
                        bind_compositor))
    return std::unexpected(
        Error::wayland_init("Failed to create wl_compositor global"));

  // Pass InputTranslator* so bind_seat can wire get_pointer/get_keyboard
  if (!wl_global_create(display, &wl_seat_interface, 7, &input_translator,
                        bind_seat))
    return std::unexpected(
        Error::wayland_init("Failed to create wl_seat global"));

  // Owned by the wl_output global from here on. libwayland has no destructor
  // hook for a global's user data, so -- exactly as register_data_device_manager
  // does for its own state -- the delete hangs off the display instead.
  // Without it this is never freed.
  auto *owner = new OutputGlobal{};
  owner->state = output_state;
  if (!wl_global_create(display, &wl_output_interface, 3, owner, bind_output)) {
    delete owner;
    return std::unexpected(
        Error::wayland_init("Failed to create wl_output global"));
  }
  owner->display_destroy.notify = [](wl_listener *listener, void *) {
    OutputGlobal *o = nullptr;
    o = wl_container_of(listener, o, display_destroy);
    // Normally every client is gone by now (the display's deleter reaps them
    // first), but a wl_output a client still holds must not find this freed.
    for (wl_resource *r : o->bound)
      wl_resource_set_user_data(r, nullptr);
    delete o;
  };
  wl_display_add_destroy_listener(display, &owner->display_destroy);

  if (!wl_global_create(display, &wl_subcompositor_interface, 1,
                        &surface_manager, bind_subcompositor))
    return std::unexpected(
        Error::wayland_init("Failed to create wl_subcompositor global"));

  WW_LOG_INFO("[wayland] Core globals registered (wl_compositor v5, wl_seat "
              "v7, wl_output v3, wl_subcompositor v1)");
  return owner;
}

} // namespace ww
