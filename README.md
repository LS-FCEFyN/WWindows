# WWindows — Wayland on X Windows

WWindows is a lightweight **Wayland-to-X11 compatibility compositor**. It lets Wayland applications run on an existing X11 desktop by translating their Wayland surfaces, input, and presentation into X11 windows and requests.

It is conceptually XWayland in reverse:

```text
XWayland:   X11 application  → XWayland  → Wayland compositor → display
WWindows:   Wayland client   → WWindows → existing X11 server → display
```

WWindows does not create a new desktop session or implement a scene graph/compositing pass. The existing X server remains responsible for displaying the windows, and **i3 is the supported window-manager integration target**.

> **Project status:** Implementation is complete for the intended i3-focused scope. All runtime verification tasks have been addressed.

## Features

- **Wayland client support:** Core surface, shared-memory buffer, seat, output, and `xdg-shell` toplevel functionality. **Touch input (`wl_touch`) is not implemented** (no test hardware available). **Only server-side decorations are supported** (via `zxdg_decoration_manager_v1`); client-side decorations are not implemented.
- **X11 window integration:** Creates and manages X11 windows for Wayland surfaces and integrates with i3 through its IPC interface.
- **Keyboard and pointer input:** Translates X11 input to Wayland events, using XKB/libxkbcommon for keymap and keyboard-state handling. Client cursor surfaces are translated to X11 cursors.
- **Multiple presentation paths:**
  - `wl_shm` buffers are presented through XShm.
  - Supported RGB dmabufs use DRI3 import and X11 Present/Render facilities when available.
  - Linear two-plane NV12 dmabufs are converted to XRGB on the CPU before presentation.
- **Buffer scaling and geometry:** Integer `wl_surface.set_buffer_scale` handling, window-geometry cropping, and **fractional scaling via `wp_fractional_scale_manager_v1` + `wp_viewporter`** are supported by the implemented presentation paths.
- **Clipboard integration:** Bridges Wayland MIME types and X11 selection targets, including ICCCM `INCR` and `MULTIPLE` transfers. Buffered clipboard payloads are capped at 64 MiB.
- **Surface and frame lifecycle:** Handles buffer detachment, retained presentation images, asynchronous imports, and frame-callback completion/cancellation. Shared-memory buffers are stride-validated and repacked when necessary; failed XShm submissions complete their callback path instead of waiting forever for a completion event.
- **Testable architecture:** Separates core logic and input handling from the concrete X11 adapter through ports and adapters, with portable tests and runtime integration suites.

## Scope and limitations

WWindows intentionally targets **i3** rather than providing generic integration with every X11 window manager. The current implementation has these known boundaries:

- It exposes a single output at a fixed 1:1 logical-to-pixel scale. It does not implement a dynamic multi-output model.
- The dmabuf compatibility path supports selected RGB formats and **linear two-plane NV12**. Other multi-plane YUV formats and non-linear NV12 modifiers are not advertised. NV12 conversion is a CPU path, not zero-copy.
- Popup constraint adjustment, popup grabs, and placement beyond the parent window are incomplete; X11 child-window popups can be clipped by their parent.
- Rotated/flipped buffer transforms are not currently supported end to end.
- `xdg_toplevel.show_window_menu` remains a deliberate no-op: i3 has no equivalent built-in window-menu request to forward.
- Frame callbacks follow backend presentation/transfer completion. This is not a guarantee of physical display-vblank timing, and WWindows does not run a separate vblank-locked rendering loop.
- **Screensharing/portal protocols** (`ext_foreign_toplevel_list_v1`, `ext_image_capture_source_v1`, `ext_image_copy_capture_v1`) are implemented but live interoperability with xdg-desktop-portal or similar consumers is not yet verified.

These boundaries are documented so clients and contributors can distinguish implemented behavior from future compatibility work.

## Requirements

- CMake 3.25 or newer.
- A C++23 compiler and standard library that provide `<print>` (for example, GCC 14 or newer with its matching libstdc++).
- An active X11 session and an i3 session for the intended window-manager integration.
- Wayland server development files, Wayland protocol definitions/tools, XCB extension libraries, and XKB libraries.

On Debian or Ubuntu, install the core build dependencies with:

```bash
sudo apt install build-essential cmake pkg-config \
  libwayland-dev wayland-protocols \
  libxcb1-dev libxcb-shm0-dev libxcb-dri3-dev libxcb-present-dev \
  libxcb-xfixes0-dev libxcb-shape0-dev libxcb-render0-dev \
  libxcb-xkb-dev libxcb-randr0-dev libxkbcommon-dev libxkbcommon-x11-dev
```

For example, when GCC 14 is installed:

```bash
cmake -S . -B build \
  -DCMAKE_C_COMPILER=gcc-14 \
  -DCMAKE_CXX_COMPILER=g++-14
cmake --build build -j"$(nproc)"
```

Wayland protocol headers and sources are generated in the build directory by `wayland-scanner`; they are not checked into the repository.

## Running

Start WWindows from the X11/i3 session:

```bash
./build/"Wayland on X Windows"
```

The startup log reports the Wayland socket name, for example `wayland-0`. In the terminal from which you want to launch Wayland clients, use that name and the same `XDG_RUNTIME_DIR` as the running WWindows process:

```bash
# If XDG_RUNTIME_DIR was unset when WWindows started, its fallback is:
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/wwindows-runtime-$(id -u)}"
export WAYLAND_DISPLAY=wayland-0  # replace with the socket name in WWindows' log
foot
```

The value of `WAYLAND_DISPLAY` set inside WWindows does **not** propagate back to the shell that launched it. Set it in the client-launching environment as shown above. If your session already defines `XDG_RUNTIME_DIR`, keep using that same directory instead of the fallback.

The executable's CMake target remains `wwindows` for build-system compatibility; the produced binary is named `Wayland on X Windows`.

Logging can be adjusted with `WWINDOWS_LOG_LEVEL`:

```bash
WWINDOWS_LOG_LEVEL=debug ./build/"Wayland on X Windows"
```

Supported levels are `trace`, `debug`, `info`, `warn`, and `error`; the default is `info`.

## Testing

Run the configured test suite with:

```bash
ctest --test-dir build --output-on-failure
```

The runtime suite includes tests for core surface behavior, Wayland globals, input translation and keymaps, the i3 adapter, and X11 integration. Some integration tests have additional environment requirements:

- **Xvfb** enables the isolated X11 backend and transient-window integration suites. If it is unavailable, those suites report as skipped rather than pretending an X11 integration test ran.
- **GBM and a DRI3-capable GPU-backed X server** are required for the real dmabuf/DRI3 test suite. It is skipped when the necessary environment is unavailable.

For the Xvfb and DRI3 test helpers:

```bash
sudo apt install xvfb libgbm-dev
```

`libgbm-dev` is optional for the normal runtime build and enables compilation of the DRI3 integration test when available.

### Portable tests only

To build the platform-independent test tier without Wayland/XCB runtime dependencies:

```bash
cmake -S . -B build-portable \
  -DWWINDOWS_BUILD_RUNTIME=OFF \
  -DWWINDOWS_BUILD_TESTS=ON
cmake --build build-portable -j"$(nproc)"
ctest --test-dir build-portable --output-on-failure
```

### Sanitizers

AddressSanitizer and UndefinedBehaviorSanitizer can be enabled with:

```bash
cmake -S . -B build-asan -DWWINDOWS_SANITIZE=ON
cmake --build build-asan -j"$(nproc)"
ctest --test-dir build-asan --output-on-failure
```

## Architecture

- [`ARCHITECTURE.md`](ARCHITECTURE.md) describes module boundaries, dependency rules, resource ownership, the X11 backend’s subsystem state split, and the testing strategy.

## License

WWindows is licensed under the **GNU General Public License, version 2 only (GPL-2.0-only)**. You may copy, redistribute, and modify it under the terms of that license. The full license text is provided in [`LICENSE`](LICENSE).
