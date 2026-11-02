#pragma once

// Pure resize / configure decisions for toplevel surfaces.
//
// The state machine around a toplevel's size distinguishes four facts that
// look alike but have different owners:
//
//   * the X11 window's size            -- owned by X (and the WM); we only
//                                         observe it, or record what we asked
//                                         X for until the echo arrives;
//   * the last xdg_toplevel.configure  -- Wayland protocol state (what the
//                                         client was told, and with which
//                                         states);
//   * the client's committed size      -- what the client actually drew;
//   * the WM's latest target           -- the last size the WM asked for.
//
// Each is its own std::optional here, so "not known yet" is a type-level
// state rather than a bool next to a pair of zeroes, and the decisions below
// take them as explicit inputs and return a verdict without touching any
// surface state. SurfaceManager applies the side effects.

#include <cstdint>
#include <optional>
#include <vector>

namespace ww::core::policy {

struct WindowSize {
  uint16_t width{0};
  uint16_t height{0};
  bool operator==(const WindowSize &) const = default;
  [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

// What the last xdg_toplevel.configure said. States are part of the identity:
// a toplevel can go maximized -> tiled at an unchanged size and that
// configure must still reach the client.
struct ConfiguredState {
  WindowSize size;
  std::vector<uint32_t> states;
  bool operator==(const ConfiguredState &) const = default;
};

// A configure is needed unless the client already has exactly this size and
// exactly these states.
[[nodiscard]] inline bool
configure_needed(const std::optional<ConfiguredState> &last, WindowSize size,
                 const std::vector<uint32_t> &states) {
  return !last || last->size != size || last->states != states;
}

// How a freshly committed client size relates to what X and the WM want.
enum class CommitVerdict {
  MatchesX11,      // client already agrees with the actual X11 window
  MatchesWmTarget, // client agrees with the WM's target (X11 may lag/differ)
  FirstFrame,      // first mapped frame: it establishes the window size
  Stale,           // client disagrees; keep stretching until it catches up
};

struct CommitInputs {
  WindowSize committed;
  std::optional<WindowSize> x11;
  WindowSize target; // WM target if one was observed, else the last configure
  bool had_previous_commit{false};
  bool was_waiting_for_frame{false};
  bool wm_owns_geometry{false};
};

[[nodiscard]] inline CommitVerdict classify_commit(const CommitInputs &in) {
  if (in.x11 && *in.x11 == in.committed)
    return CommitVerdict::MatchesX11;
  if (in.committed == in.target)
    return CommitVerdict::MatchesWmTarget;
  if (!in.had_previous_commit && !in.was_waiting_for_frame &&
      !in.wm_owns_geometry)
    return CommitVerdict::FirstFrame;
  return CommitVerdict::Stale;
}

// A client that commits a size other than the one it was configured to,
// while something other than the client owns the size, has to be told the
// size once more: nothing else will, and stretching its buffer forever is the
// only alternative. (foot's resize-keep-grid does exactly this on a font zoom
// even when tiled.) Floating windows never need it -- the client's size is
// adopted instead.
//
// Bounded so a client that cannot comply cannot start a configure/commit
// loop: one re-assert per distinct committed size, remembered until the client
// obeys.
struct ReassertInputs {
  WindowSize committed;
  WindowSize target;
  bool size_is_enforced{false};      // WM-owned geometry, or the WM overrode us
  bool all_configures_acked{false};  // the client has seen the latest configure
  std::optional<WindowSize> already_reasserted_against;
};

[[nodiscard]] inline bool should_reassert_configure(const ReassertInputs &in) {
  return in.size_is_enforced && in.all_configures_acked &&
         !in.target.empty() && !in.committed.empty() &&
         in.committed != in.target &&
         in.already_reasserted_against != in.committed;
}

} // namespace ww::core::policy
