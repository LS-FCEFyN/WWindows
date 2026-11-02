// Tests for the keymap the compositor actually hands its clients.
//
// This was a known gap: the InputTranslator suite asserted on
// the evdev offset (`keycode - 8`) but used whatever keymap the test machine
// happened to resolve by default, so nothing checked that the keymap itself
// was right -- or even that it described the layout the compositor was
// configured for. A machine set to AZERTY and a machine set to QWERTY would
// both pass, which means neither was being tested.
//
// Two things make that testable here:
//
//   * The layout is *pinned per test* through XKB_DEFAULT_LAYOUT rather than
//     inherited. xkb_context_new(XKB_CONTEXT_NO_FLAGS) reads the
//     XKB_DEFAULT_* environment at context creation, and InputTranslator
//     creates its context in set_display(), so setting the variable before
//     constructing the translator selects the layout with no production
//     change and no test-only seam.
//
//   * The keymap is read back *as the client receives it*. wl_keyboard.keymap
//     delivers a file descriptor out of band via SCM_RIGHTS; the fixture now
//     captures it, so a test can mmap that descriptor, compile it with
//     xkbcommon exactly as a real client would, and ask what a keycode
//     actually means.
//
// The pair of layouts below is the point. Asserting only that QWERTY
// produces 'a' would pass against a compositor that ignored configuration
// entirely and always shipped a default keymap. Asserting that the *same
// keycode* yields 'a' under `us` and 'q' under `fr` cannot.

#include "../fakes/fake_surface_interactions.hpp"
#include "../fakes/fake_window_backend.hpp"
#include "../fakes/wayland_fixture.hpp"
#include "../support/test.hpp"

#include <wwindows/input/input_translator.hpp>

#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

using ww_test::FakeSurfaceInteractions;
using ww_test::FakeWindowBackend;
using ww_test::WaylandFixture;

namespace {

constexpr uint16_t kKeyboardKeymap = 0; // wl_keyboard.keymap opcode
constexpr uint32_t kWindow = 42;

// X11 keycode 38 is evdev KEY_A (30) + 8 -- the home-row key immediately
// right of Caps Lock. QWERTY calls it 'a'; AZERTY calls it 'q'. Keycode 39
// is 's' on both, which makes it a useful control: it shows the two keymaps
// are genuinely different layouts rather than two arbitrary compilations.
constexpr xkb_keycode_t kKeycodeA = 38;
constexpr xkb_keycode_t kKeycodeS = 39;

// Sets XKB_DEFAULT_LAYOUT for its lifetime and restores whatever was there
// before, so one test's layout cannot leak into the next.
class ScopedLayout {
public:
  explicit ScopedLayout(const char *layout) {
    if (const char *prev = std::getenv("XKB_DEFAULT_LAYOUT")) {
      had_previous_ = true;
      previous_ = prev;
    }
    ::setenv("XKB_DEFAULT_LAYOUT", layout, 1);
    // Variants and options are inherited from the developer's session
    // otherwise, and a stray XKB_DEFAULT_VARIANT would silently change what
    // these keycodes mean.
    if (const char *prev = std::getenv("XKB_DEFAULT_VARIANT")) {
      had_previous_variant_ = true;
      previous_variant_ = prev;
    }
    ::unsetenv("XKB_DEFAULT_VARIANT");
  }
  ~ScopedLayout() {
    if (had_previous_)
      ::setenv("XKB_DEFAULT_LAYOUT", previous_.c_str(), 1);
    else
      ::unsetenv("XKB_DEFAULT_LAYOUT");
    if (had_previous_variant_)
      ::setenv("XKB_DEFAULT_VARIANT", previous_variant_.c_str(), 1);
  }
  ScopedLayout(const ScopedLayout &) = delete;
  ScopedLayout &operator=(const ScopedLayout &) = delete;

private:
  bool had_previous_{false};
  bool had_previous_variant_{false};
  std::string previous_;
  std::string previous_variant_;
};

// Sets XKB_DEFAULT_VARIANT for its lifetime and restores whatever was there
// before. Deliberately separate from ScopedLayout (which unconditionally
// clears this same variable so a stray inherited variant can't leak into a
// layout-only test) rather than folding a variant parameter into it: a test
// that wants a variant constructs ScopedLayout first, then this, so the
// order documents which one wins and ScopedLayout's own clearing runs
// before this sets the real value.
class ScopedVariant {
public:
  explicit ScopedVariant(const char *variant) {
    if (const char *prev = std::getenv("XKB_DEFAULT_VARIANT")) {
      had_previous_ = true;
      previous_ = prev;
    }
    ::setenv("XKB_DEFAULT_VARIANT", variant, 1);
  }
  ~ScopedVariant() {
    if (had_previous_)
      ::setenv("XKB_DEFAULT_VARIANT", previous_.c_str(), 1);
    else
      ::unsetenv("XKB_DEFAULT_VARIANT");
  }
  ScopedVariant(const ScopedVariant &) = delete;
  ScopedVariant &operator=(const ScopedVariant &) = delete;

private:
  bool had_previous_{false};
  std::string previous_;
};

// Sets XKB_DEFAULT_OPTIONS for its lifetime and restores whatever was there
// before. Nothing else in this file touches this variable, so a value
// inherited from the developer's own session could otherwise leak in
// unnoticed; every test that cares about options sets its own via this.
class ScopedOptions {
public:
  explicit ScopedOptions(const char *options) {
    if (const char *prev = std::getenv("XKB_DEFAULT_OPTIONS")) {
      had_previous_ = true;
      previous_ = prev;
    }
    ::setenv("XKB_DEFAULT_OPTIONS", options, 1);
  }
  ~ScopedOptions() {
    if (had_previous_)
      ::setenv("XKB_DEFAULT_OPTIONS", previous_.c_str(), 1);
    else
      ::unsetenv("XKB_DEFAULT_OPTIONS");
  }
  ScopedOptions(const ScopedOptions &) = delete;
  ScopedOptions &operator=(const ScopedOptions &) = delete;

private:
  bool had_previous_{false};
  std::string previous_;
};

struct Harness {
  FakeWindowBackend backend;
  FakeSurfaceInteractions surfaces;
  ww::InputTranslator input{backend, surfaces};
  WaylandFixture wl; // destroyed first -- see input_translator_tests.cpp

  wl_resource *keyboard{nullptr};

  Harness() {
    input.set_display(wl.display());
    wl_resource *surface = wl.surface();
    surfaces.bind(kWindow, surface);
    keyboard = wl.keyboard();
    input.add_keyboard(keyboard);
  }
};

// The keymap exactly as the client would reconstruct it: take the fd from
// the wl_keyboard.keymap event, mmap it, and compile.
struct ClientKeymap {
  xkb_context *ctx{nullptr};
  xkb_keymap *keymap{nullptr};
  xkb_state *state{nullptr};

  ~ClientKeymap() {
    if (state)
      xkb_state_unref(state);
    if (keymap)
      xkb_keymap_unref(keymap);
    if (ctx)
      xkb_context_unref(ctx);
  }
  ClientKeymap() = default;
  ClientKeymap(const ClientKeymap &) = delete;
  ClientKeymap &operator=(const ClientKeymap &) = delete;

  [[nodiscard]] bool valid() const { return state != nullptr; }

  [[nodiscard]] xkb_keysym_t sym(xkb_keycode_t keycode) const {
    return state ? xkb_state_key_get_one_sym(state, keycode) : XKB_KEY_NoSymbol;
  }

  [[nodiscard]] std::string sym_name(xkb_keycode_t keycode) const {
    char buf[64]{};
    xkb_keysym_get_name(sym(keycode), buf, sizeof buf);
    return buf;
  }
};

// Reads the keymap the compositor sent on `keyboard`, by occurrence:
// occurrence 0 is the first wl_keyboard.keymap event ever sent to it,
// occurrence 1 the second (e.g. after a keymap change), and so on.
// Returns an invalid ClientKeymap if there is no such occurrence or it
// could not be compiled. Assumes `keyboard` is the only resource this
// fixture has sent any fd to -- true for every test in this file, which
// each use a single-keyboard Harness (the multi-keyboard test reads
// occurrence 0 on each of two keyboards, never a later occurrence on
// one after fds have gone to the other).
std::unique_ptr<ClientKeymap> receive_keymap(WaylandFixture &wl,
                                             wl_resource *keyboard,
                                             std::size_t occurrence = 0) {
  auto out = std::make_unique<ClientKeymap>();

  const auto events = wl.events_for(keyboard);
  std::optional<uint32_t> size;
  std::optional<uint32_t> format;
  std::size_t seen = 0;
  for (const auto &e : events) {
    if (e.opcode != kKeyboardKeymap || e.args.size() < 8)
      continue;
    if (seen != occurrence) {
      ++seen;
      continue;
    }
    format = WaylandFixture::arg_u32(e, 0);
    // The fd is *not* in args -- it travels as ancillary data -- so the
    // second inline argument is the size.
    size = WaylandFixture::arg_u32(e, 1);
    break;
  }
  if (!size || !format || *format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
    return out;

  const auto &fds = wl.received_fds();
  if (fds.size() <= occurrence)
    return out;
  const int fd = fds[occurrence];

  void *addr = ::mmap(nullptr, *size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED)
    return out;

  out->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (out->ctx) {
    out->keymap = xkb_keymap_new_from_string(
        out->ctx, static_cast<const char *>(addr),
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (out->keymap)
      out->state = xkb_state_new(out->keymap);
  }
  ::munmap(addr, *size);
  return out;
}

} // namespace

// --- the keymap is sent at all, and is well-formed --------------------------

WW_TEST_CASE(keyboard_receives_a_compilable_xkb_v1_keymap) {
  ScopedLayout layout("us");
  Harness h;

  const auto km = receive_keymap(h.wl, h.keyboard);
  // A keymap the client cannot compile is indistinguishable, from the
  // client's side, from no keyboard at all.
  WW_TEST_CHECK(km->valid());
}

WW_TEST_CASE(keymap_event_carries_the_xkb_v1_format_and_a_descriptor) {
  ScopedLayout layout("us");
  Harness h;

  const auto events = h.wl.events_for(h.keyboard);
  bool found = false;
  for (const auto &e : events) {
    if (e.opcode != kKeyboardKeymap)
      continue;
    found = true;
    WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(e, 0),
                     uint32_t{WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1});
    // Size covers the keymap text plus its null terminator; anything
    // near-zero means the client would mmap nothing useful.
    WW_TEST_CHECK(WaylandFixture::arg_u32(e, 1) > 100u);
  }
  WW_TEST_CHECK(found);
  WW_TEST_CHECK(!h.wl.received_fds().empty());
}

// --- the keymap describes the configured layout -----------------------------

WW_TEST_CASE(qwerty_layout_maps_the_home_row_keycode_to_a) {
  ScopedLayout layout("us");
  Harness h;

  const auto km = receive_keymap(h.wl, h.keyboard);
  WW_TEST_CHECK(km->valid());
  if (km->valid())
    WW_TEST_CHECK_EQ(km->sym_name(kKeycodeA), std::string("a"));
}

WW_TEST_CASE(azerty_layout_maps_the_same_keycode_to_q) {
  ScopedLayout layout("fr");
  Harness h;

  const auto km = receive_keymap(h.wl, h.keyboard);
  WW_TEST_CHECK(km->valid());
  if (km->valid())
    WW_TEST_CHECK_EQ(km->sym_name(kKeycodeA), std::string("q"));
}

WW_TEST_CASE(configured_layout_changes_what_a_keycode_means) {
  // The differential form, and the one that actually pins the behavior: a
  // compositor that ignored XKB configuration and always shipped one
  // built-in keymap would satisfy either absolute test above on the machine
  // where that default happened to match, but cannot satisfy this one.
  std::string qwerty_a;
  std::string azerty_a;
  std::string qwerty_s;
  std::string azerty_s;

  {
    ScopedLayout layout("us");
    Harness h;
    const auto km = receive_keymap(h.wl, h.keyboard);
    WW_TEST_CHECK(km->valid());
    if (km->valid()) {
      qwerty_a = km->sym_name(kKeycodeA);
      qwerty_s = km->sym_name(kKeycodeS);
    }
  }
  {
    ScopedLayout layout("fr");
    Harness h;
    const auto km = receive_keymap(h.wl, h.keyboard);
    WW_TEST_CHECK(km->valid());
    if (km->valid()) {
      azerty_a = km->sym_name(kKeycodeA);
      azerty_s = km->sym_name(kKeycodeS);
    }
  }

  WW_TEST_CHECK(!qwerty_a.empty());
  WW_TEST_CHECK(!azerty_a.empty());
  WW_TEST_CHECK(qwerty_a != azerty_a);
  // ...while a key the two layouts agree on stays put, which is what shows
  // the difference above is a layout change and not two unrelated keymaps.
  WW_TEST_CHECK_EQ(qwerty_s, azerty_s);
}

// --- every client gets the same keymap --------------------------------------

WW_TEST_CASE(a_second_keyboard_receives_the_same_keymap) {
  ScopedLayout layout("us");
  Harness h;

  wl_resource *second = h.wl.keyboard();
  h.input.add_keyboard(second);

  // Two keyboards, two keymap events, two descriptors. A compositor that
  // sent the keymap only to the first client would leave later ones unable
  // to interpret any key event they receive.
  WW_TEST_CHECK_EQ(h.wl.count_events(second, kKeyboardKeymap), std::size_t{1});
  WW_TEST_CHECK(h.wl.received_fds().size() >= 2);

  const auto first_km = receive_keymap(h.wl, h.keyboard);
  WW_TEST_CHECK(first_km->valid());
  if (first_km->valid())
    WW_TEST_CHECK_EQ(first_km->sym_name(kKeycodeA), std::string("a"));
}

// --- a keymap change is redistributed, not just compiled once ---------------
//
// The startup race this covers: keyboard_keymap_string() is only ever a
// snapshot of whatever the session's layout is *at the moment it's asked*.
// If this process starts before whatever configures the real layout has
// run -- an i3 exec/exec_always line, ~/.xprofile, xorg.conf's XkbLayout --
// the very first fetch simply gets the wrong answer, same as it would for
// any live setxkbmap while running. IWindowBackend::InputCallbacks::
// on_xkb_keymap_changed exists to correct that: re-fetch, recompile, and
// redistribute to every client already holding a keyboard, whenever the
// backend reports the server's keymap changed underneath it (XkbMapNotify
// in the real X11 backend). These tests fire that callback directly,
// exactly as input_translator_tests.cpp already does for on_key et al.,
// rather than needing a real X server to generate a genuine XkbMapNotify.

WW_TEST_CASE(a_keymap_change_sends_a_second_keymap_event) {
  ScopedLayout layout("us");
  Harness h;
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKeymap),
                   std::size_t{1});

  ScopedLayout new_layout("fr");
  h.backend.input_cbs.on_xkb_keymap_changed();

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKeymap),
                   std::size_t{2});
}

WW_TEST_CASE(the_redistributed_keymap_describes_the_new_layout) {
  ScopedLayout layout("us");
  Harness h;
  const auto before = receive_keymap(h.wl, h.keyboard, /*occurrence=*/0);
  WW_TEST_CHECK(before->valid());
  if (before->valid())
    WW_TEST_CHECK_EQ(before->sym_name(kKeycodeA), std::string("a"));

  ScopedLayout new_layout("fr");
  h.backend.input_cbs.on_xkb_keymap_changed();

  // Before the fix, this event simply never arrived -- a client that
  // compiled the first keymap once at bind time and never expected
  // another would keep interpreting every keystroke under the stale
  // layout for the rest of the session.
  const auto after = receive_keymap(h.wl, h.keyboard, /*occurrence=*/1);
  WW_TEST_CHECK(after->valid());
  if (after->valid())
    WW_TEST_CHECK_EQ(after->sym_name(kKeycodeA), std::string("q"));
}

WW_TEST_CASE(a_keymap_change_reaches_every_bound_keyboard) {
  ScopedLayout layout("us");
  Harness h;
  wl_resource *second = h.wl.keyboard();
  h.input.add_keyboard(second);
  WW_TEST_CHECK_EQ(h.wl.count_events(second, kKeyboardKeymap),
                   std::size_t{1});

  ScopedLayout new_layout("fr");
  h.backend.input_cbs.on_xkb_keymap_changed();

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKeymap),
                   std::size_t{2});
  WW_TEST_CHECK_EQ(h.wl.count_events(second, kKeyboardKeymap),
                   std::size_t{2});
}

WW_TEST_CASE(a_no_op_keymap_change_does_not_redistribute) {
  // keyboard_keymap_string() always returns nullopt in tests (no real X
  // session to ask -- see FakeWindowBackend's comment), so every
  // recompile in this file falls back to the same
  // xkb_keymap_new_from_names(ctx, nullptr, ...) path. With the layout
  // unchanged between the two calls, that fallback compiles the *same*
  // keymap both times; on_xkb_keymap_changed is expected to notice
  // (keymap_str == previous_keymap) and skip redistributing a keymap
  // nothing actually changed.
  ScopedLayout layout("us");
  Harness h;
  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKeymap),
                   std::size_t{1});

  h.backend.input_cbs.on_xkb_keymap_changed();

  WW_TEST_CHECK_EQ(h.wl.count_events(h.keyboard, kKeyboardKeymap),
                   std::size_t{1});
}

// --- variants, options, and multi-group layouts ------------------------
//
// The two layouts above (us/fr) show the *layout* field of XKB_DEFAULT_*
// reaches the compiled keymap. Nothing so far shows the same for variant,
// options, or a layout list long enough to have more than one group --
// each goes through xkb_keymap_new_from_names the same way layout does
// (recompile_keymap()'s fallback passes rule_names=nullptr, so xkbcommon
// reads every one of XKB_DEFAULT_RULES/MODEL/LAYOUT/VARIANT/OPTIONS off
// the environment itself), but a bug specific to one of them -- e.g. a
// future change that threaded layout through explicitly while leaving
// the others on some other path -- would not be caught by a test that
// only ever varies layout.

WW_TEST_CASE(a_variant_changes_the_layout_while_the_base_stays_the_same) {
  // us/dvorak: the physical key one to the right of home-row-left (evdev
  // KEY_S, keycode 39) moves from 's' to 'o'. Keycode 38 (KEY_A) is a
  // deliberate control -- Dvorak happens to keep it 'a' too, which is
  // what shows this is a variant of `us` and not some unrelated layout
  // substituted in by accident.
  ScopedLayout layout("us");
  ScopedVariant variant("dvorak");
  Harness h;

  const auto km = receive_keymap(h.wl, h.keyboard);
  WW_TEST_CHECK(km->valid());
  if (km->valid()) {
    WW_TEST_CHECK_EQ(km->sym_name(kKeycodeA), std::string("a"));
    WW_TEST_CHECK_EQ(km->sym_name(kKeycodeS), std::string("o"));
  }
}

WW_TEST_CASE(an_option_reaches_the_compiled_keymap) {
  // caps:swapescape swaps Escape and Caps Lock -- a clean yes/no check
  // that needs no second keymap to compare against, unlike layout/variant
  // above: keycode 9 (Escape) simply must or must not be Caps_Lock.
  ScopedLayout layout("us");
  ScopedOptions options("caps:swapescape");
  Harness h;

  const auto km = receive_keymap(h.wl, h.keyboard);
  WW_TEST_CHECK(km->valid());
  if (km->valid()) {
    constexpr xkb_keycode_t kKeycodeEscape = 9;
    WW_TEST_CHECK_EQ(km->sym_name(kKeycodeEscape), std::string("Caps_Lock"));
  }
}

WW_TEST_CASE(a_two_group_layout_lets_the_group_argument_select_between_them) {
  // XKB_DEFAULT_LAYOUT accepts a comma-separated list, one group per
  // entry -- how a real multi-layout session (e.g. a physical keyboard
  // shortcut toggling between two input languages) is configured. This
  // is the compositor-independent half of group switching: that the
  // *keymap itself* contains both groups and answers correctly when
  // asked for one or the other. The other half -- that this compositor
  // actually tells clients which group is active via wl_keyboard.
  // modifiers -- is xkb_state_changed_updates_the_active_group_directly
  // below.
  ScopedLayout layout("us,fr");
  Harness h;

  const auto km = receive_keymap(h.wl, h.keyboard);
  WW_TEST_CHECK(km->valid());
  if (!km->valid())
    return;

  // XKB StateNotify exposes a single effective group. libxkbcommon says to
  // mirror a single reported group through locked_layout, with the depressed
  // and latched layout components set to zero.
  xkb_state_update_mask(km->state, /*depressed_mods=*/0, /*latched_mods=*/0,
                        /*locked_mods=*/0, /*depressed_group=*/0,
                        /*latched_group=*/0, /*locked_group=*/0);
  WW_TEST_CHECK_EQ(km->sym_name(kKeycodeA), std::string("a"));

  xkb_state_update_mask(km->state, 0, 0, 0, 0, 0, 1);
  WW_TEST_CHECK_EQ(km->sym_name(kKeycodeA), std::string("q"));
}

WW_TEST_CASE(xkb_state_changed_updates_the_active_group_directly) {
  // The runtime half of multi-group switching -- that this compositor
  // forwards whatever group the X server reports via wl_keyboard.
  // modifiers' group argument. Three groups are intentional here: with a
  // two-group keymap, passing the effective group as all three components
  // accidentally still selects group 1 (1 + 1 + 1 wraps to 1 modulo 2).
  // On a three-group keymap that mistake wraps to group 0, so this test pins
  // "us,fr,de" to catch the incorrect replication of the effective group.
  ScopedLayout layout("us,fr,de");
  Harness h;

  h.backend.input_cbs.on_xkb_state_changed(/*depressed=*/0, /*latched=*/0,
                                           /*locked=*/0, /*group=*/1);

  // A key event alone establishes focus and triggers send_modifiers --
  // see input_translator_tests.cpp's
  // a_key_event_establishes_focus_with_no_prior_focus_in for why no
  // separate enter is needed first.
  h.backend.input_cbs.on_key(kWindow, /*keycode=*/65, /*pressed=*/true,
                             /*time=*/0);

  constexpr uint16_t kKeyboardModifiers = 4;
  const auto events = h.wl.events_for(h.keyboard);
  const WaylandFixture::Event *mods = nullptr;
  for (const auto &e : events)
    if (e.opcode == kKeyboardModifiers)
      mods = &e;
  WW_TEST_CHECK(mods != nullptr);
  // args: serial, mods_depressed, mods_latched, mods_locked, group.
  if (mods)
    WW_TEST_CHECK_EQ(WaylandFixture::arg_u32(*mods, 4), uint32_t{1});
}
