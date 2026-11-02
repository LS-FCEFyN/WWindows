#pragma once

#include <memory>
#include <string_view>
#include <wwindows/support/error.hpp>

struct wl_display;

namespace ww {

class EventLoop;
class SurfaceManager;
class InputTranslator;
struct OutputState;

class WaylandServer {
public:
  ~WaylandServer();
  WaylandServer(const WaylandServer &) = delete;
  WaylandServer &operator=(const WaylandServer &) = delete;
  WaylandServer(WaylandServer &&) = delete;
  WaylandServer &operator=(WaylandServer &&) = delete;

  static Result<std::unique_ptr<WaylandServer>>
  create(EventLoop &loop, SurfaceManager &surface_manager,
         InputTranslator &input_translator, const OutputState &output_state);

  // The window system changed the screen (resolution, physical size, refresh
  // rate): tell every client holding a wl_output, and refresh the preferred
  // fractional scale, which is derived from the same numbers (DPI). Does
  // nothing if no client-visible value changed.
  void update_output(const OutputState &state);

  [[nodiscard]] wl_display *display() const;
  [[nodiscard]] std::string_view socket_name() const;

private:
  WaylandServer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ww