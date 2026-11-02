#pragma once

// A recording in-memory IWindowBackend.
//
// This is the payoff the ports-and-adapters split is supposed to buy: the
// entire core surface state machine can be exercised with no X server, no
// XCB connection, and no GPU, by substituting this for X11Backend. Without
// a fake like this the port is just an interface nobody benefits from --
// the architecture document claims a backend can be swapped out, and this
// is the test that proves it.
//
// Every call is recorded so tests can assert on the *sequence* of backend
// operations a protocol interaction produces, not just the end state.

#include <wwindows/ports/window_backend.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ww_test {

struct FakeWindowBackend final : ww::IWindowBackend {
  // --- recorded state ------------------------------------------------------
  struct Window {
    uint32_t id{0};
    uint32_t parent{0};
    int16_t x{0};
    int16_t y{0};
    uint16_t width{0};
    uint16_t height{0};
    bool mapped{false};
    std::string title;
    // Window identity and hints, as last set through the port.
    std::string wm_class_instance;
    std::string wm_class;
    ww::WindowSizeLimits size_limits;
    uint32_t transient_for{0};
    uint32_t client_pid{0};
  };

  std::unordered_map<uint32_t, Window> windows;
  std::vector<std::string> calls;      // ordered call log, e.g. "create_window"
  std::unordered_set<uint32_t> destroyed;   // ids passed to destroy_window
  std::unordered_set<uint32_t> forgotten;   // ids passed to forget_window
  std::vector<uint32_t> presented_pixmaps;
  std::pair<uint16_t, uint16_t> last_stretch_size{0, 0};
  std::unordered_set<uint32_t> retained_frames;
  std::vector<uint32_t> repainted_windows_with_frame;
  bool defer_import_callbacks{false};
  bool defer_presented_callbacks{false};
  std::vector<DmabufImportCallback> pending_import_callbacks;
  std::vector<FramePresentedCallback> pending_presented_callbacks;
  bool defer_source_consumed_callbacks{false};
  std::vector<FramePresentedCallback> pending_source_consumed_callbacks;

  void finish_import(std::size_t index, uint32_t pixmap) {
    auto callback = std::move(pending_import_callbacks.at(index));
    pending_import_callbacks.erase(pending_import_callbacks.begin() +
                                   static_cast<std::ptrdiff_t>(index));
    if (callback)
      callback(pixmap);
  }

  void finish_next_source_consumed() {
    auto callback = std::move(pending_source_consumed_callbacks.front());
    pending_source_consumed_callbacks.erase(pending_source_consumed_callbacks.begin());
    if (callback)
      callback();
  }

  void finish_next_presentation() {
    auto callback = std::move(pending_presented_callbacks.front());
    pending_presented_callbacks.erase(pending_presented_callbacks.begin());
    if (callback)
      callback();
  }

  void complete_or_defer(FramePresentedCallback callback) {
    if (!callback)
      return;
    if (defer_presented_callbacks)
      pending_presented_callbacks.push_back(std::move(callback));
    else
      callback();
  }
  // The window-geometry crop each present was asked for, in call order.
  std::vector<ww::BufferRegion> presented_regions;
  uint32_t last_input_shape_window{0};
  bool last_input_shape_has_region{false};
  std::vector<ww::InputRect> last_input_shape_add;
  std::vector<ww::InputRect> last_input_shape_sub;
  struct StateRequest {
    uint32_t window{0};
    ww::WindowState state{};
    bool enabled{false};
  };
  std::vector<StateRequest> window_states;
  // request_interactive_move/resize and minimize_window calls, in order.
  std::vector<uint32_t> interactive_moves;   // window ids
  struct ResizeRequest {
    uint32_t window{0};
    ww::ResizeEdge edge{};
  };
  std::vector<ResizeRequest> interactive_resizes;
  std::vector<uint32_t> minimized_windows;   // window ids
  std::vector<std::string> clipboard_mimes;
  std::optional<dev_t> main_device{}; // std::nullopt => no dmabuf device

  ResizeCallback resize_cb;
  WmStateCallback wm_state_cb;
  CloseCallback close_cb;
  InputCallbacks input_cbs;
  ClipboardSendCallback clipboard_send_cb;
  ClipboardOwnerChangedCallback clipboard_owner_changed_cb;

  uint32_t next_id{100};

  // --- helpers used by tests ----------------------------------------------
  [[nodiscard]] bool alive(uint32_t id) const {
    return windows.find(id) != windows.end();
  }
  [[nodiscard]] const Window *window(uint32_t id) const {
    auto it = windows.find(id);
    return it == windows.end() ? nullptr : &it->second;
  }
  [[nodiscard]] std::size_t count(std::string_view name) const {
    return static_cast<std::size_t>(
        std::count(calls.begin(), calls.end(), std::string{name}));
  }
  [[nodiscard]] bool called(std::string_view name) const {
    return count(name) > 0;
  }
  void clear_calls() { calls.clear(); }

  // --- IWindowBackend ------------------------------------------------------
  uint32_t create_window(uint16_t width, uint16_t height) override {
    calls.emplace_back("create_window");
    const uint32_t id = next_id++;
    windows[id] = Window{.id = id, .width = width, .height = height,
                         .title = {}, .wm_class_instance = {},
                         .wm_class = {}, .size_limits = {}};
    return id;
  }

  uint32_t create_child_window(uint32_t parent, int16_t x, int16_t y,
                               uint16_t width, uint16_t height) override {
    calls.emplace_back("create_child_window");
    const uint32_t id = next_id++;
    windows[id] = Window{.id = id, .parent = parent, .x = x, .y = y,
                         .width = width, .height = height, .title = {}, .wm_class_instance = {},
                         .wm_class = {}, .size_limits = {}};
    return id;
  }

  // --- pointer cursors -------------------------------------------------------
  // Cursor handles live in their own id space (starting well clear of window
  // ids) so a test can tell a cursor from a window at a glance.
  struct CreatedCursor {
    uint32_t id{0};
    uint16_t width{0};
    uint16_t height{0};
    int16_t hotspot_x{0};
    int16_t hotspot_y{0};
    std::vector<uint32_t> pixels; // tightly packed, as presented
  };
  std::vector<CreatedCursor> created_cursors;
  std::vector<uint32_t> freed_cursors;
  // window id -> the cursor last set on it (0 = default / inherited).
  std::unordered_map<uint32_t, uint32_t> window_cursors;
  uint32_t next_cursor_id{9000};
  uint32_t blank_cursor_id{0};
  bool cursor_creation_fails{false};

  uint32_t create_cursor(const ww::CursorImage &image) override {
    calls.emplace_back("create_cursor");
    if (cursor_creation_fails)
      return 0;
    CreatedCursor c{.id = next_cursor_id++,
                    .width = image.width,
                    .height = image.height,
                    .hotspot_x = image.hotspot_x,
                    .hotspot_y = image.hotspot_y,
                    .pixels = {}};
    const auto *rows = static_cast<const uint8_t *>(image.argb);
    for (uint16_t y = 0; y < image.height; ++y)
      for (uint16_t x = 0; x < image.width; ++x) {
        uint32_t px = 0;
        std::memcpy(&px, rows + static_cast<std::size_t>(y) * image.stride + x * 4u, 4);
        c.pixels.push_back(px);
      }
    created_cursors.push_back(c);
    return c.id;
  }

  void free_cursor(uint32_t cursor) override {
    calls.emplace_back("free_cursor");
    freed_cursors.push_back(cursor);
  }

  uint32_t blank_cursor() override {
    calls.emplace_back("blank_cursor");
    if (!blank_cursor_id)
      blank_cursor_id = next_cursor_id++;
    return blank_cursor_id;
  }

  void set_window_cursor(uint32_t window, uint32_t cursor) override {
    calls.emplace_back("set_window_cursor");
    window_cursors[window] = cursor;
  }

  // The cursor currently on `window`, 0 when none was ever set or it was reset.
  [[nodiscard]] uint32_t cursor_of(uint32_t window) const {
    auto it = window_cursors.find(window);
    return it == window_cursors.end() ? 0 : it->second;
  }

  void move_window(uint32_t window_id, int16_t x, int16_t y) override {
    calls.emplace_back("move_window");
    if (auto it = windows.find(window_id); it != windows.end()) {
      it->second.x = x;
      it->second.y = y;
    }
  }

  struct RestackCall {
    uint32_t window_id;
    uint32_t sibling_id;
    bool above;
  };
  std::vector<RestackCall> restacks;

  void restack_window(uint32_t window_id, uint32_t sibling_id,
                      bool above) override {
    calls.emplace_back("restack_window");
    restacks.push_back({window_id, sibling_id, above});
  }

  void destroy_window(uint32_t window_id) override {
    calls.emplace_back("destroy_window");
    destroyed.insert(window_id);
    // Mirror X11: destroying a window destroys its children too. Tests that
    // assert we never double-destroy a child depend on this.
    for (auto it = windows.begin(); it != windows.end();) {
      if (it->second.parent == window_id)
        it = windows.erase(it);
      else
        ++it;
    }
    windows.erase(window_id);
  }

  void forget_window(uint32_t window_id) override {
    calls.emplace_back("forget_window");
    forgotten.insert(window_id);
    windows.erase(window_id);
  }

  void map_window(uint32_t window_id) override {
    calls.emplace_back("map_window");
    if (auto it = windows.find(window_id); it != windows.end())
      it->second.mapped = true;
  }

  void unmap_window(uint32_t window_id) override {
    calls.emplace_back("unmap_window");
    if (auto it = windows.find(window_id); it != windows.end())
      it->second.mapped = false;
  }

  void repaint_window(uint32_t window, ww::BufferRegion = {}, int32_t = 0,
                      FramePresentedCallback on_presented = {}) override {
    calls.emplace_back("repaint_window");
    if (retained_frames.contains(window))
      repainted_windows_with_frame.push_back(window);
    complete_or_defer(std::move(on_presented));
  }

  void repaint_window_scaled(uint32_t window, uint16_t width,
                             uint16_t height,
                             FramePresentedCallback on_presented = {}) override {
    calls.emplace_back("repaint_window_scaled");
    last_stretch_size = {width, height};
    if (retained_frames.contains(window))
      repainted_windows_with_frame.push_back(window);
    complete_or_defer(std::move(on_presented));
  }

  void clear_retained_frame(uint32_t window) override {
    calls.emplace_back("clear_retained_frame");
    retained_frames.erase(window);
  }

  void resize_window(uint32_t window_id, uint16_t width,
                     uint16_t height) override {
    calls.emplace_back("resize_window");
    if (auto it = windows.find(window_id); it != windows.end()) {
      it->second.width = width;
      it->second.height = height;
    }
  }

  void set_window_title(uint32_t window_id, const char *utf8_title) override {
    calls.emplace_back("set_window_title");
    if (auto it = windows.find(window_id); it != windows.end())
      it->second.title = utf8_title ? utf8_title : "";
  }

  void set_window_class(uint32_t window_id, const char *instance_name,
                        const char *class_name) override {
    calls.emplace_back("set_window_class");
    if (auto it = windows.find(window_id); it != windows.end()) {
      it->second.wm_class_instance = instance_name ? instance_name : "";
      it->second.wm_class = class_name ? class_name : "";
    }
  }

  void set_window_size_limits(uint32_t window_id,
                              const ww::WindowSizeLimits &limits) override {
    calls.emplace_back("set_window_size_limits");
    if (auto it = windows.find(window_id); it != windows.end())
      it->second.size_limits = limits;
  }

  void set_window_transient_for(uint32_t window_id, uint32_t parent) override {
    calls.emplace_back("set_window_transient_for");
    if (auto it = windows.find(window_id); it != windows.end())
      it->second.transient_for = parent;
  }

  void set_window_client_process(uint32_t window_id, uint32_t pid) override {
    calls.emplace_back("set_window_client_process");
    if (auto it = windows.find(window_id); it != windows.end())
      it->second.client_pid = pid;
  }

  void set_input_shape(uint32_t window, bool has_region,
                       const std::vector<ww::InputRect> &add,
                       const std::vector<ww::InputRect> &sub) override {
    calls.emplace_back("set_input_shape");
    last_input_shape_window = window;
    last_input_shape_has_region = has_region;
    last_input_shape_add = add;
    last_input_shape_sub = sub;
  }

  // Test-controlled stand-in for the real keyboard: set `pressed_keycodes`
  // before triggering a focus change to exercise resync_modifiers().
  [[nodiscard]] std::vector<uint8_t> query_pressed_keycodes() override {
    calls.emplace_back("query_pressed_keycodes");
    return pressed_keycodes;
  }
  std::vector<uint8_t> pressed_keycodes;

  // No real X session to ask in tests — nullopt exercises exactly the
  // same "fall back to a default keymap" path a real server without the
  // XKB extension would.
  // What the "server" reports as its keyboard repeat setting; nullopt = it
  // cannot say (the default, like keyboard_keymap_string below).
  std::optional<KeyRepeat> key_repeat;
  [[nodiscard]] std::optional<KeyRepeat> keyboard_repeat() override {
    calls.emplace_back("keyboard_repeat");
    return key_repeat;
  }

  [[nodiscard]] std::optional<std::string> keyboard_keymap_string() override {
    calls.emplace_back("keyboard_keymap_string");
    return std::nullopt;
  }

  // The buffer width/height each present_pixels call was given, in order.
  std::vector<std::pair<uint16_t, uint16_t>> presented_sizes;

  // The pixels each present_pixels call was given, tightly re-read as 32-bit
  // words (row padding dropped), so tests can assert on what reached the
  // screen and not only on how big it was.
  struct PresentedPixels {
    uint16_t width{0};
    uint16_t height{0};
    uint32_t stride{0};
    std::vector<uint32_t> pixels;
  };
  std::vector<PresentedPixels> presented_pixels;

  void present_pixels(uint32_t window, uint16_t width, uint16_t height,
                      uint32_t stride, const void *pixels,
                      ww::BufferRegion src = {},
                      FramePresentedCallback on_presented = {}) override {
    calls.emplace_back("present_pixels");
    presented_sizes.emplace_back(width, height);
    {
      PresentedPixels copy{width, height, stride, {}};
      copy.pixels.reserve(std::size_t{width} * height);
      const auto *rows = static_cast<const uint8_t *>(pixels);
      for (uint32_t y = 0; pixels && y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
          uint32_t word = 0;
          std::memcpy(&word, rows + std::size_t{y} * stride + std::size_t{x} * 4,
                      4);
          copy.pixels.push_back(word);
        }
      presented_pixels.push_back(std::move(copy));
    }
    presented_regions.push_back(src);
    retained_frames.insert(window);
    complete_or_defer(std::move(on_presented));
  }

  void import_dmabuf(int, uint32_t, uint16_t, uint16_t, uint8_t, uint8_t,
                     uint16_t, uint64_t, bool,
                     DmabufImportCallback on_imported) override {
    calls.emplace_back("import_dmabuf");
    // Contract: fires exactly once and may be asynchronous. 0 == import failed.
    if (on_imported) {
      if (defer_import_callbacks)
        pending_import_callbacks.push_back(std::move(on_imported));
      else
        on_imported(next_id++);
    }
  }

  void present_pixmap(uint32_t window, uint32_t pixmap,
                      ww::BufferRegion src = {}, int32_t = 1,
                      FramePresentedCallback on_presented = {},
                      FramePresentedCallback on_source_consumed = {},
                      uint16_t stretch_width = 0,
                      uint16_t stretch_height = 0) override {
    calls.emplace_back(stretch_width || stretch_height
                           ? "present_pixmap_scaled" : "present_pixmap");
    last_stretch_size = {stretch_width, stretch_height};
    presented_pixmaps.push_back(pixmap);
    presented_regions.push_back(src);
    retained_frames.insert(window);
    if (on_source_consumed) {
      if (defer_source_consumed_callbacks)
        pending_source_consumed_callbacks.push_back(std::move(on_source_consumed));
      else
        on_source_consumed();
    }
    complete_or_defer(std::move(on_presented));
  }

  void set_window_state(uint32_t window_id, ww::WindowState state,
                        bool enabled) override {
    calls.emplace_back("set_window_state");
    window_states.push_back({window_id, state, enabled});
  }

  void request_interactive_move(uint32_t window_id) override {
    calls.emplace_back("request_interactive_move");
    interactive_moves.push_back(window_id);
  }

  void request_interactive_resize(uint32_t window_id,
                                  ww::ResizeEdge edge) override {
    calls.emplace_back("request_interactive_resize");
    interactive_resizes.push_back({window_id, edge});
  }

  void minimize_window(uint32_t window_id) override {
    calls.emplace_back("minimize_window");
    minimized_windows.push_back(window_id);
  }

  void present_nv12(uint32_t window, uint16_t, uint16_t, int, uint32_t,
                    uint32_t, int, uint32_t, uint32_t,
                    ww::BufferRegion src = {}, int32_t = 1,
                    FramePresentedCallback on_presented = {},
                    uint16_t stretch_width = 0,
                    uint16_t stretch_height = 0) override {
    calls.emplace_back(stretch_width || stretch_height
                           ? "present_nv12_scaled" : "present_nv12");
    last_stretch_size = {stretch_width, stretch_height};
    presented_regions.push_back(src);
    retained_frames.insert(window);
    complete_or_defer(std::move(on_presented));
  }

  void free_pixmap(uint32_t) override { calls.emplace_back("free_pixmap"); }

  std::optional<dev_t> dmabuf_main_device() const override {
    return main_device;
  }

  void set_resize_callback(ResizeCallback cb) override {
    resize_cb = std::move(cb);
  }
  void set_wm_state_callback(WmStateCallback cb) override {
    wm_state_cb = std::move(cb);
  }
  void set_close_callback(CloseCallback cb) override {
    close_cb = std::move(cb);
  }
  void set_input_callbacks(InputCallbacks cbs) override {
    input_cbs = std::move(cbs);
  }
  void set_events_processed_callback(std::function<void()>) override {}

  void set_clipboard_owner(std::vector<std::string> mime_types) override {
    calls.emplace_back("set_clipboard_owner");
    clipboard_mimes = std::move(mime_types);
  }
  void set_clipboard_send_callback(ClipboardSendCallback cb) override {
    clipboard_send_cb = std::move(cb);
  }
  void set_clipboard_owner_changed_callback(
      ClipboardOwnerChangedCallback cb) override {
    clipboard_owner_changed_cb = std::move(cb);
  }
  void request_clipboard_data(const std::string &,
                              ClipboardDataCallback on_data) override {
    calls.emplace_back("request_clipboard_data");
    if (on_data)
      on_data({});
  }
};

} // namespace ww_test
