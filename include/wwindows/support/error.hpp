#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace ww {

enum class ErrorCode {
  WaylandInitFailed,
  WaylandSocketFailed,
  X11ConnectFailed,
  X11ScreenFailed,
  BindingFailed,
  Unknown,
};

struct Error {
  ErrorCode code;
  std::string message;

  // Named constructors — call sites read clearly
  static Error wayland_init(std::string_view msg) {
    return {ErrorCode::WaylandInitFailed, std::string{msg}};
  }
  static Error wayland_socket(std::string_view msg) {
    return {ErrorCode::WaylandSocketFailed, std::string{msg}};
  }
  static Error x11_connect(std::string_view msg) {
    return {ErrorCode::X11ConnectFailed, std::string{msg}};
  }
  static Error x11_screen(std::string_view msg) {
    return {ErrorCode::X11ScreenFailed, std::string{msg}};
  }
  static Error unknown(std::string_view msg) {
    return {ErrorCode::Unknown, std::string{msg}};
  }
};

// Every fallible function returns one of these
template <typename T> using Result = std::expected<T, Error>;

// For functions that succeed with no value
using VoidResult = std::expected<void, Error>;

} // namespace ww