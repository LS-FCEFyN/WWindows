# WWindows architecture

## Architectural style

WWindows follows a **ports-and-adapters (hexagonal) architecture**. The
application/core side owns behavior and abstractions; operating-system and
protocol integrations sit behind adapters.

```text
                     +------------------------+
                     |    WWindows facade     |
                     |  (composition root)    |
                     +-----------+------------+
                                 |
             +-------------------+-------------------+
             |                                       |
      +------v------+                         +------v------+
      |    Core     |   ISurfaceInteractions  |    Input    |
      | surfaces /  |<------------------------| translation |
      | event loop  |                         +------+------+
      +------+------+                                |
             |                                       |
       +-----v---------------------------------------v-----+
       |              IWindowBackend (port)                |
       +-----+---------------------------------------+-----+
             |                                       |
      +------v------+                         +------v------+
      | X11 adapter |                         |   Wayland   |
      | XCB / DRI3  |                         |  protocol   |
      | Present/XShm|                         |   adapter   |
      +-------------+                         +-------------+
```

## Layers and dependency direction

| Layer | Depends on | Must never depend on |
|---|---|---|
| `support/` | nothing | anything |
| `ports/` | `support/` | any adapter |
| `core/` | `ports/`, `support/`, libwayland | XCB, xkbcommon, X11 adapter |
| `input/` | `ports/`, `support/`, libwayland, xkb | XCB, X11 adapter, **`core/`** |
| `adapters/*` | `ports/`, `core/`, platform libs | each other |

The window-manager adapters (`adapters/i3/`) sit behind
`IWindowManagerWatcher`: they report *why* a toplevel has the geometry it
has (tiled vs floating) and never change it. Only `src/application.cpp`
wires one in, after reading the socket path the X11 adapter found on the
root window.

The same fact has a second, WM-agnostic source: `IWindowBackend::WmWindowState`
(`set_wm_state_callback`) reports the standard EWMH `_NET_WM_STATE`
maximize/fullscreen atoms, which `X11Backend` fills in for any window
manager and `SurfaceManager` wires up unconditionally alongside i3.
| `application` | everything | — |

`input/` deliberately does not link `core/`: it reaches surfaces through the
narrow `ISurfaceInteractions` port instead of the full compositor state
machine. This is enforced three ways — by the CMake target graph, by
`tests/architecture/check_dependencies.py`, and by the fact that the input
library links without `wwindows_core` present.

## Design patterns

**Ports and Adapters** — `IWindowBackend` is the primary outbound port;
`X11Backend` is one adapter. `tests/fakes/fake_window_backend.hpp` is a second,
which is what makes the claim testable rather than aspirational.

**Facade** — `WWindows` is the public lifecycle surface for applications.

**Strategy** — presentation/input behavior is selected through the backend
port, so another backend can be substituted without changing core code.

**Observer** — asynchronous backend events (resize, close, input, clipboard
ownership) are delivered through callbacks rather than polling.

**PImpl** — platform-heavy implementations stay out of stable public headers.

**RAII** — file descriptors, X11/Wayland lifetimes, and imported buffers have
single ownership.

## X11 adapter implementation state

The public X11 backend uses PImpl: `X11Backend::Impl` is declared in
`include/wwindows/adapters/x11/detail/x11_backend_impl.hpp`. It is the
composition root for shared connection/event-loop state and five subsystem
state aggregates, each defined in a neighboring `detail/` header:

| State aggregate | Ownership boundary | Main implementation units |
|---|---|---|
| `ClipboardState` | ICCCM selection ownership/requesting, MIME mappings, INCR and MULTIPLE transfers | `X11Backend_clipboard.cpp`, `X11Backend_events.cpp` |
| `PresentState` | DRI3 imports, Present events/callbacks, Render pictures, retained frames and fences | `X11Backend_dri3.cpp`, `X11Backend_present.cpp`, `X11Backend_render.cpp`, `X11Backend_events.cpp` |
| `ShmBackendState` | MIT-SHM segments and bounded pending frames, per window | `X11Backend_present.cpp`, `X11Backend_events.cpp` |
| `WindowBackendState` | visuals, WM/EWMH atoms, GCs/depths, transient relationships and WM-state reads | `X11Backend_window.cpp`, `X11Backend_lifecycle.cpp`, `X11Backend_events.cpp` |
| `InputBackendState` | XKB setup, deferred modifier snapshots and input callbacks | `X11Backend_lifecycle.cpp`, `X11Backend_events.cpp`, `X11Backend_callbacks.cpp` |

These aggregates are data holders, not new backend objects. Event dispatch and
state-machine methods remain on `X11Backend::Impl`, because one event pass
coordinates multiple subsystems. The grouping makes ownership and teardown
responsibilities visible without creating cross-adapter dependencies.

The SHM presenter records each `PutImage` request sequence. An asynchronous X
error can therefore retire the exact failed segment and complete its callback
when the server will not send the normal SHM completion event. Incoming Wayland
SHM rows are validated against the advertised four-byte pixel formats and
repacked when their legal byte stride cannot be represented exactly by XShm's
whole-pixel pitch. Clipboard INCR transfers are bounded in size and time.

## Policy modules

Pure decisions live in `*/policy/` components so they can be unit-tested with
no X server and no Wayland runtime:

- surface geometry clamping (`core/policy/surface_geometry.hpp`);
- pointer button and scroll-axis mapping (`input/policy/pointer_mapping.hpp`);
- clipboard MIME selection (`adapters/x11/policy/clipboard_mime.hpp`);
- fractional-scale calculation (`adapters/wayland/policy/scale.hpp`).

**Production code calls these directly.** A policy is not allowed to be a
parallel reimplementation kept only for the benefit of the tests: the
architecture check fails the build if any policy stops being reachable from
`src/`. That rule exists because four of them previously *were* unreferenced,
so their tests reported coverage the compositor did not have — and in one case
(`clamp_size`) the unused policy was guarding a defect that was live in the
commit path.

## Lifetime and destruction order

Destruction order is load-bearing and fixed in `src/application.cpp`:

```text
InputTranslator -> WaylandServer -> SurfaceManager -> X11Backend -> EventLoop
```

`SurfaceManager` and `InputTranslator` register `wl_resource` destroy
listeners and do **not** unregister them in their destructors. The
`wl_display` must therefore be torn down while those objects are still alive,
which is why `WaylandServer` is destroyed first. `WlDisplayDeleter` calls
`wl_display_destroy_clients()` before `wl_display_destroy()`, because the
latter alone leaves connected clients' resources allocated and their destroy
listeners unfired.

`tests/core/surface_manager_tests.cpp` encodes this in its `Harness` and pins
it with `display_teardown_before_the_manager_is_safe`; the reverse order is a
use-after-free under ASan.

## Testing strategy

Suites are split by module rather than one monolithic binary:

| Suite | Needs | Covers |
|---|---|---|
| `wwindows_test_support` | nothing | `Error`, `UniqueFd` |
| `wwindows_test_ports` | nothing | port interfaces are implementable |
| `wwindows_test_core` | nothing | `EventLoop`, `SurfaceIndex`, geometry policy |
| `wwindows_test_input` | nothing | pointer/scroll mapping policy |
| `wwindows_test_x11` | nothing | clipboard MIME policy |
| `wwindows_test_wayland` | libwayland | fractional-scale policy |
| `wwindows_test_surface_manager` | libwayland | the real surface state machine |
| `wwindows_test_input_translator` | libwayland, xkbcommon | the real input translator |
| `wwindows_test_i3` | nothing | i3 framing/JSON policy, `I3Watcher` against a fake i3 socket |
| `wwindows_test_x11_backend` | Xvfb (skips cleanly without it) | the real X11 adapter, against a live server |
| `wwindows_architecture_check` | python3 | the rules on this page |

The three behavioral suites are the substantive ones — they drive production
code, not reimplementations of it, against fakes or a real server:

- **`wwindows_test_surface_manager`** drives `SurfaceManager` against
  `FakeWindowBackend` (a second `IWindowBackend`, recording every call) and a
  real in-process `wl_display` (a `wl_client` over a socketpair). Asserts on
  the *sequence of backend calls* a protocol interaction produces.
- **`wwindows_test_input_translator`** drives `InputTranslator` through the
  same `IWindowBackend::InputCallbacks` that `X11Backend` fires in
  production, against `FakeSurfaceInteractions` and the same `wl_display`
  fixture, extended with wire-level readback (`WaylandFixture::events_for`)
  since pointer/keyboard events have no other observable effect to assert on.
- **`wwindows_test_x11_backend`** runs against a real, throwaway Xvfb
  server rather than a mocked XCB layer — an X11 adapter is almost entirely
  a conversation with the server, so mocking XCB would mostly assert that
  the code calls the functions it calls. State is verified through an
  independent second XCB connection
  (`tests/x11/x11_backend_tests.cpp`'s `Inspector`), so a backend that
  merely believes it created a window cannot pass.

Together these are the port's conformance specification: the backend-call
sequence `FakeWindowBackend` records for a given protocol interaction is the
contract a second `IWindowBackend` implementation — including the real
`X11Backend`, verified independently by its own suite — has to satisfy.

Tests are self-registering: defining one adds it to the suite, and a suite
that registers zero tests fails rather than silently passing.

## Dependency graph

```text
wwindows (composition root)
    ├── core ──────> core_portable + ports + protocols
    ├── input ─────> ports + protocols        (never core)
    ├── x11 adapter ──> core_portable + ports + XCB/DRI3/Present/XShm
    └── wayland adapter ──> core + input + ports + libwayland
```

`core_portable` holds the platform-free part of core (`EventLoop`,
`SurfaceIndex`) and is compiled once, then shared by the runtime and the
tests, so the two cannot drift apart.
