# Agent Instructions for WWindows

## Documentation Files to Read

Before working on this codebase, agents **must** read the following documentation files to understand the architecture, implementation status, and project conventions:

### Required Reading (in order)

1. **README.md** — Project overview, features, requirements, running instructions, testing, and license
2. **ARCHITECTURE.md** — Module boundaries, dependency rules, resource ownership, X11 backend subsystem state split, testing strategy
3. **LICENSE** — GPL-2.0-only license terms

### Project Scope

- **Target**: i3 window manager only (other WMs explicitly out of scope unless the user specifically asks for it)
- **Architecture**: Ports-and-adapters (hexagonal) — core logic separated from X11/Wayland adapters
- **Languages**: C++23 (core), C11 (wayland-scanner generated code)
- **Build**: CMake 3.25+, `-Werror` strict warnings

### Implemented Wayland Protocols

**Core (fully implemented):**
- `wl_compositor`, `wl_surface`, `wl_region`, `wl_subcompositor`, `wl_output`
- `wl_seat` with `wl_pointer`, `wl_keyboard` — **`wl_touch` NOT implemented** (no test hardware)
- `wl_data_device_manager`, `wl_data_device`, `wl_data_offer`, `wl_data_source` (clipboard + drag-and-drop)
- `xdg_wm_base`, `xdg_surface`, `xdg_toplevel`, `xdg_popup`, `xdg_positioner`

**Fractional Scaling / HiDPI:**
- `wp_viewporter` (`wp_viewport` with `set_source`/`set_destination`)
- `wp_fractional_scale_manager_v1` (`wp_fractional_scale_v1` with `preferred_scale`)

**Screensharing / Portals:**
- `ext_foreign_toplevel_list_v1` (enumerate toplevels)
- `ext_image_capture_source_v1` (capture sessions/streams)
- `ext_image_copy_capture_v1` (frame copy to `wl_shm` buffer)

**Other:**
- `zwp_linux_dmabuf_v1` (dmabuf import via DRI3/Present/Render)
- `zxdg_decoration_manager_v1` (server-side decorations)

### Key Implementation Files

- `src/main.cpp` — Entry point, signal handlers (SIGTERM/SIGHUP for logout cleanup)
- `src/application.cpp` — Composition root, destruction order
- `src/adapters/wayland/WaylandServer.cpp` — Registers all Wayland globals
- `src/adapters/x11/X11Backend_lifecycle.cpp` — X11 connection, IoErr/IoHup handling
- `src/core/surface_manager.cpp` — Wayland surface state machine
- `src/input/input_translator.cpp` — X11 → Wayland input translation

### Testing

Run tests with: `ctest --test-dir build --output-on-failure`

**Test suites (15 executables, each running many internal test cases):**
- Portable: support, ports, core, input, x11
- Runtime: wayland, globals, surface_manager, input_translator, xkb_keymap, i3, x11_backend (Xvfb), transient_for_e2e (Xvfb), dri3 (GPU)
- Architecture: check_dependencies.py

### Conventions

- No `-Wno-error` flags — all warnings are errors
- ASan/UBSan enabled via `-DWWINDOWS_SANITIZE=ON`
- Self-registering tests via `test_main.cpp` (fails if suite has zero tests)
- Destruction order: InputTranslator → WaylandServer → SurfaceManager → X11Backend → EventLoop
- Wayland fd integration uses `IoErr` handling for graceful shutdown
- X11 fd integration uses `IoErr | IoHup` handling for logout cleanup

### Do Not

- Add build artifacts, Docker files, or generated files to version control
- Implement `wl_touch` without test hardware
- Implement window managers other than i3 (unless the user specifically asks for it)