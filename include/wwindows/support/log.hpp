#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string_view>
#include <utility>

// ---------------------------------------------------------------------------
// A minimal leveled logger.
//
// Every call site picks a level (trace/debug/info/warn/error) describing how
// noteworthy the event is on its own — not how it happened to help debug one
// particular bug. That keeps logging "error-agnostic": the same lines are
// useful whether nothing is wrong or something is, because they describe
// state and lifecycle rather than a specific failure's symptoms.
//
// - trace  — per-event/per-frame noise (pointer motion, present vsync
//            events, protocol dispatch). Off by default; only useful when
//            actively chasing something and the volume is acceptable.
// - debug  — moderate-frequency detail (resource create/destroy, protocol
//            requests). Off by default, on for `WWINDOWS_LOG_LEVEL=debug`.
// - info   — subsystem lifecycle: startup, shutdown, global registration.
//            Default level; low enough volume to always be on.
// - warn   — recoverable/degraded conditions (falling back to a slower
//            path, dropping a frame, rejecting a malformed request).
// - error  — a fallible operation failed outright.
//
// Level is controlled at runtime via WWINDOWS_LOG_LEVEL=trace|debug|info|
// warn|error (default: info), read once at first use.
// ---------------------------------------------------------------------------

namespace ww::log {

enum class Level { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };

namespace detail {

inline Level level_from_env() {
  const char *env = std::getenv("WWINDOWS_LOG_LEVEL");
  if (!env)
    return Level::Info;
  const std::string_view v{env};
  if (v == "trace")
    return Level::Trace;
  if (v == "debug")
    return Level::Debug;
  if (v == "info")
    return Level::Info;
  if (v == "warn" || v == "warning")
    return Level::Warn;
  if (v == "error")
    return Level::Error;
  return Level::Info;
}

inline std::atomic<Level> &min_level() {
  static std::atomic<Level> level{level_from_env()};
  return level;
}

inline constexpr std::string_view tag(Level lvl) {
  switch (lvl) {
  case Level::Trace:
    return "TRACE";
  case Level::Debug:
    return "DEBUG";
  case Level::Info:
    return "INFO ";
  case Level::Warn:
    return "WARN ";
  case Level::Error:
    return "ERROR";
  }
  return "?????";
}

// warn/error go to stderr so they show up even if stdout is redirected to a
// log file that's only sampled occasionally; everything else goes to stdout.
inline std::FILE *stream_for(Level lvl) {
  return lvl >= Level::Warn ? stderr : stdout;
}

} // namespace detail

// Override the level programmatically (e.g. a future --log-level flag).
inline void set_level(Level lvl) { detail::min_level().store(lvl); }

inline bool enabled(Level lvl) {
  return lvl >= detail::min_level().load(std::memory_order_relaxed);
}

template <typename... Args>
void log(Level lvl, std::format_string<Args...> fmt, Args &&...args) {
  if (!enabled(lvl))
    return;
  std::FILE *f = detail::stream_for(lvl);
  std::print(f, "[{}] ", detail::tag(lvl));
  std::println(f, fmt, std::forward<Args>(args)...);
}

} // namespace ww::log

#define WW_LOG_TRACE(...) ::ww::log::log(::ww::log::Level::Trace, __VA_ARGS__)
#define WW_LOG_DEBUG(...) ::ww::log::log(::ww::log::Level::Debug, __VA_ARGS__)
#define WW_LOG_INFO(...) ::ww::log::log(::ww::log::Level::Info, __VA_ARGS__)
#define WW_LOG_WARN(...) ::ww::log::log(::ww::log::Level::Warn, __VA_ARGS__)
#define WW_LOG_ERROR(...) ::ww::log::log(::ww::log::Level::Error, __VA_ARGS__)
