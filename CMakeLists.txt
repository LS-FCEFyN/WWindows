cmake_minimum_required(VERSION 3.25)
project(wwindows VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
option(WWINDOWS_BUILD_RUNTIME "Build the Wayland/X11 runtime" ON)
option(WWINDOWS_BUILD_TESTS   "Build unit/module test suites" ${BUILD_TESTING})
option(WWINDOWS_SANITIZE      "Build with ASan+UBSan (tests and runtime)" OFF)

# ---------------------------------------------------------------------------
# Architecture
# ---------------------------------------------------------------------------
# Ports-and-adapters / hexagonal boundary:
#
#   support        no dependencies at all (Error, Log, UniqueFd)
#   ports          abstract outbound interfaces; depends on support only
#   core_portable  platform-free core logic (EventLoop, SurfaceIndex, policies)
#   core           core_portable + Wayland protocol state machine
#   input          ports + Wayland/xkb; never links core
#   adapters/*     concrete implementations of the ports
#   application    the only translation unit that names a concrete adapter
#
# Design patterns used deliberately:
#   * Ports & Adapters: IWindowBackend isolates X11 from core/input.
#   * Strategy: presentation/input behavior enters through the backend port.
#   * Facade: WWindows is the public lifecycle API.
#   * Observer: backend callbacks deliver asynchronous events without coupling.
#   * PImpl: platform-heavy implementations stay outside public headers.
#   * RAII: every OS resource has a single owner.
#
# Policy classes under */policy are pure functions. Production code calls
# them directly -- they are not parallel reimplementations kept only for the
# tests, and tests/architecture/check_dependencies.py fails the build if a
# policy ever becomes unreferenced by production code.
# ---------------------------------------------------------------------------

# --- Shared compile settings ------------------------------------------------
add_library(wwindows_warnings INTERFACE)
target_compile_options(wwindows_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wunused -Woverloaded-virtual -Wconversion
    -Wsign-conversion -Wnull-dereference -Wdouble-promotion
    -Wformat=2 -Wimplicit-fallthrough -Werror)
target_include_directories(wwindows_warnings INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

if(WWINDOWS_SANITIZE)
  target_compile_options(wwindows_warnings INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(wwindows_warnings INTERFACE
      -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Platform dependency discovery
#
# Resolved up front so every module below can state its real dependencies at
# the point it is declared, instead of being declared first and linked later.
# ---------------------------------------------------------------------------
if(WWINDOWS_BUILD_RUNTIME)
  find_package(PkgConfig REQUIRED)

  pkg_check_modules(WAYLAND_SERVER    REQUIRED IMPORTED_TARGET wayland-server)
  # Client library: used only by the test suite, to drive the protocol
  # globals with a real client connection. Optional so the runtime still
  # configures on a machine that has only the server development files.
  pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
  pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
  pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

  pkg_check_modules(XCB         REQUIRED IMPORTED_TARGET xcb)
  pkg_check_modules(XCB_SHM     REQUIRED IMPORTED_TARGET xcb-shm)
  pkg_check_modules(XCB_DRI3    REQUIRED IMPORTED_TARGET xcb-dri3)
  pkg_check_modules(XCB_PRESENT REQUIRED IMPORTED_TARGET xcb-present)
  pkg_check_modules(XCB_XFIXES  REQUIRED IMPORTED_TARGET xcb-xfixes)
  pkg_check_modules(XCB_SHAPE   REQUIRED IMPORTED_TARGET xcb-shape)
  pkg_check_modules(XCB_RENDER  REQUIRED IMPORTED_TARGET xcb-render)
  pkg_check_modules(XCB_XKB     REQUIRED IMPORTED_TARGET xcb-xkb)
  pkg_check_modules(XCB_RANDR   REQUIRED IMPORTED_TARGET xcb-randr)
  pkg_check_modules(XKBCOMMON   REQUIRED IMPORTED_TARGET xkbcommon)
  pkg_check_modules(XKBCOMMON_X11 REQUIRED IMPORTED_TARGET xkbcommon-x11)

  # Some libwayland versions add optional destructor requests to core
  # protocol interfaces. Detect them at configure time so Compositor.cpp can
  # use fully-designated initializers either way, rather than guessing a
  # libwayland version number.
  include(CheckStructHasMember)
  set(CMAKE_REQUIRED_INCLUDES ${WAYLAND_SERVER_INCLUDE_DIRS})
  check_struct_has_member("struct wl_compositor_interface" release
      wayland-server-protocol.h WWINDOWS_HAVE_WL_COMPOSITOR_RELEASE LANGUAGE C)
  check_struct_has_member("struct wl_surface_interface" get_release
      wayland-server-protocol.h WWINDOWS_HAVE_WL_SURFACE_GET_RELEASE LANGUAGE C)
  check_struct_has_member("struct wl_data_device_manager_interface" release
      wayland-server-protocol.h WWINDOWS_HAVE_WL_DATA_DEVICE_MANAGER_RELEASE LANGUAGE C)
  unset(CMAKE_REQUIRED_INCLUDES)

  # --- Wayland protocol codegen -> wwindows_protocols ----------------------
  # One library boundary for every generated protocol source: consumers link
  # it and get the generated headers on their include path. No module needs
  # to know wayland-scanner was involved.
  set(WWINDOWS_PROTOCOL_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(PROTOCOL_OUTPUT_DIR
      "${WWINDOWS_PROTOCOL_BUILD_DIR}/wwindows/adapters/wayland/protocols")
  file(MAKE_DIRECTORY "${PROTOCOL_OUTPUT_DIR}")

  set(PROTO_HEADERS)
  set(PROTO_SOURCES)
  macro(wwindows_generate_protocol OUTPUT_STEM INPUT_XML)
    set(_header "${PROTOCOL_OUTPUT_DIR}/${OUTPUT_STEM}-server-protocol.h")
    set(_client_header "${PROTOCOL_OUTPUT_DIR}/${OUTPUT_STEM}-client-protocol.h")
    set(_source "${PROTOCOL_OUTPUT_DIR}/${OUTPUT_STEM}-protocol.c")
    add_custom_command(OUTPUT "${_header}"
      COMMAND ${WAYLAND_SCANNER} server-header < "${INPUT_XML}" > "${_header}"
      DEPENDS "${INPUT_XML}"
      COMMENT "wayland-scanner server-header ${OUTPUT_STEM}")
    # The client header is generated for the test suite only: driving the
    # protocol globals with a real wl_client issuing real requests is the
    # only way to cover the request-unpacking half of those handlers. The
    # runtime never includes it -- the same generated .c serves both sides,
    # so there is no second copy of the marshalling tables to drift.
    add_custom_command(OUTPUT "${_client_header}"
      COMMAND ${WAYLAND_SCANNER} client-header < "${INPUT_XML}" > "${_client_header}"
      DEPENDS "${INPUT_XML}"
      COMMENT "wayland-scanner client-header ${OUTPUT_STEM}")
    add_custom_command(OUTPUT "${_source}"
      COMMAND ${WAYLAND_SCANNER} private-code < "${INPUT_XML}" > "${_source}"
      DEPENDS "${INPUT_XML}"
      COMMENT "wayland-scanner private-code ${OUTPUT_STEM}")
    list(APPEND PROTO_HEADERS "${_header}" "${_client_header}")
    list(APPEND PROTO_SOURCES "${_source}")
  endmacro()

  # Use local protocol XML files to ensure consistent versions across build
  # environments. These are vendored from upstream wayland-protocols.
  set(WWINDOWS_PROTOCOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/wayland/protocols")
  wwindows_generate_protocol(xdg-shell
      "${WWINDOWS_PROTOCOL_DIR}/xdg-shell.xml")
  wwindows_generate_protocol(linux-dmabuf-unstable-v1
      "${WWINDOWS_PROTOCOL_DIR}/linux-dmabuf-unstable-v1.xml")
  wwindows_generate_protocol(zxdg-decoration-unstable-v1
      "${WWINDOWS_PROTOCOL_DIR}/zxdg-decoration-unstable-v1.xml")
  wwindows_generate_protocol(fractional-scale-v1
      "${WWINDOWS_PROTOCOL_DIR}/fractional-scale-v1.xml")
  wwindows_generate_protocol(viewporter
      "${WWINDOWS_PROTOCOL_DIR}/viewporter.xml")
  wwindows_generate_protocol(foreign-toplevel-list
      "${WWINDOWS_PROTOCOL_DIR}/foreign-toplevel-list.xml")
  wwindows_generate_protocol(image-capture-source
      "${WWINDOWS_PROTOCOL_DIR}/image-capture-source.xml")
  wwindows_generate_protocol(image-copy-capture
      "${WWINDOWS_PROTOCOL_DIR}/image-copy-capture.xml")

  add_custom_target(wwindows_generate_protocols
      DEPENDS ${PROTO_HEADERS} ${PROTO_SOURCES})

  add_library(wwindows_protocols STATIC ${PROTO_SOURCES})
  add_dependencies(wwindows_protocols wwindows_generate_protocols)
  # Generated headers are a PUBLIC part of this target's interface: anything
  # linking it can #include <wwindows/adapters/wayland/protocols/...>.
  target_include_directories(wwindows_protocols PUBLIC
      ${WWINDOWS_PROTOCOL_BUILD_DIR})
  target_link_libraries(wwindows_protocols PUBLIC PkgConfig::WAYLAND_SERVER)
  # Generated code is not ours to keep warning-clean.
  target_compile_options(wwindows_protocols PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# wwindows_core_portable -- core logic with no platform dependency
#
# Builds and runs anywhere a C++23 compiler exists: no Wayland, no XCB, no X
# server. Compiled exactly once and reused by both the runtime and the test
# suite, so the two can never drift apart.
# ---------------------------------------------------------------------------
add_library(wwindows_core_portable STATIC
    src/core/EventLoop.cpp
    src/core/SurfaceIndex.cpp)
target_link_libraries(wwindows_core_portable PUBLIC wwindows_warnings)

if(WWINDOWS_BUILD_RUNTIME)
  # --- core: surface/role state machine over the backend port --------------
  add_library(wwindows_core STATIC
      src/core/SurfaceManager_buffers.cpp
      src/core/SurfaceManager_frames.cpp
      src/core/SurfaceManager_impl.cpp
      src/core/SurfaceManager_input_region.cpp
      src/core/SurfaceManager_lifecycle.cpp
      src/core/SurfaceManager_popup.cpp
      src/core/SurfaceManager_subsurface.cpp
      src/core/SurfaceManager_toplevel.cpp)
  target_link_libraries(wwindows_core PUBLIC
      wwindows_core_portable wwindows_warnings wwindows_protocols
      PkgConfig::WAYLAND_SERVER)

  # --- input: raw backend events -> wl_pointer / wl_keyboard ---------------
  # Deliberately does NOT link wwindows_core: it reaches surfaces through the
  # narrow ISurfaceInteractions port instead. Enforced here and by
  # tests/architecture/check_dependencies.py.
  add_library(wwindows_input STATIC
      src/input/InputTranslator_drag.cpp
      src/input/InputTranslator_keyboard.cpp
      src/input/InputTranslator_lifecycle.cpp
      src/input/InputTranslator_pointer.cpp)
  target_link_libraries(wwindows_input PUBLIC
      wwindows_warnings wwindows_protocols
      PkgConfig::WAYLAND_SERVER PkgConfig::XKBCOMMON)

  # --- adapters/x11 --------------------------------------------------------
  add_library(wwindows_x11 STATIC
      src/adapters/x11/X11Backend_callbacks.cpp
      src/adapters/x11/X11Backend_clipboard.cpp
      src/adapters/x11/X11Backend_cursor.cpp
      src/adapters/x11/X11Backend_dri3.cpp
      src/adapters/x11/X11Backend_events.cpp
      src/adapters/x11/X11Backend_lifecycle.cpp
      src/adapters/x11/X11Backend_present.cpp
      src/adapters/x11/X11Backend_render.cpp
      src/adapters/x11/X11Backend_window.cpp)
  target_link_libraries(wwindows_x11 PUBLIC
      wwindows_core_portable wwindows_warnings
      PkgConfig::XCB PkgConfig::XCB_SHM PkgConfig::XCB_DRI3
      PkgConfig::XCB_PRESENT PkgConfig::XCB_XFIXES PkgConfig::XCB_SHAPE
      PkgConfig::XCB_RENDER PkgConfig::XCB_XKB PkgConfig::XCB_RANDR PkgConfig::XKBCOMMON
      PkgConfig::XKBCOMMON_X11)

  # --- adapters/i3 ---------------------------------------------------------
  # Reads i3's IPC socket to learn whether a toplevel is tiled or floating.
  # Depends on nothing but the ports and the event loop; the socket path is
  # handed in by the composition root, which is what reads it off X.
  add_library(wwindows_i3 STATIC src/adapters/i3/I3Watcher.cpp)
  target_link_libraries(wwindows_i3 PUBLIC
      wwindows_core_portable wwindows_warnings)

  # --- adapters/wayland ----------------------------------------------------
  add_library(wwindows_wayland STATIC
      src/adapters/wayland/WaylandServer.cpp
      src/adapters/wayland/FractionalScale.cpp
      src/adapters/wayland/XdgDecoration.cpp
      src/adapters/wayland/globals/viewporter/Viewporter.cpp
      src/adapters/wayland/globals/foreign_toplevel_list/ForeignToplevelList.cpp
      src/adapters/wayland/globals/image_capture_source/ImageCaptureSource.cpp
      src/adapters/wayland/globals/image_copy_capture/ImageCopyCapture.cpp
      src/adapters/wayland/globals/core/Compositor.cpp
      src/adapters/wayland/globals/core/Output.cpp
      src/adapters/wayland/globals/core/Region.cpp
      src/adapters/wayland/globals/core/Register.cpp
      src/adapters/wayland/globals/core/Seat.cpp
      src/adapters/wayland/globals/core/Subcompositor.cpp
      src/adapters/wayland/globals/core/Surface.cpp
      src/adapters/wayland/globals/data_device/Device.cpp
      src/adapters/wayland/globals/data_device/Drag.cpp
      src/adapters/wayland/globals/data_device/Manager.cpp
      src/adapters/wayland/globals/data_device/Offer.cpp
      src/adapters/wayland/globals/data_device/Selection.cpp
      src/adapters/wayland/globals/data_device/Source.cpp
      src/adapters/wayland/globals/linux_dmabuf/Buffer.cpp
      src/adapters/wayland/globals/linux_dmabuf/Dmabuf.cpp
      src/adapters/wayland/globals/linux_dmabuf/Feedback.cpp
      src/adapters/wayland/globals/linux_dmabuf/Params.cpp
      src/adapters/wayland/globals/xdg_shell/Popup.cpp
      src/adapters/wayland/globals/xdg_shell/Positioner.cpp
      src/adapters/wayland/globals/xdg_shell/Surface.cpp
      src/adapters/wayland/globals/xdg_shell/Toplevel.cpp
      src/adapters/wayland/globals/xdg_shell/WmBase.cpp
      src/adapters/wayland/policy/scale.cpp)
  target_link_libraries(wwindows_wayland PUBLIC
      wwindows_core wwindows_input wwindows_warnings wwindows_protocols
      PkgConfig::WAYLAND_SERVER PkgConfig::XKBCOMMON)

  # --- composition root ----------------------------------------------------
  # The only target that links both adapters. src/application.cpp is the only
  # translation unit in the project allowed to name X11Backend.
  add_executable(wwindows src/main.cpp src/application.cpp)
  # Keep the internal CMake target stable while giving the produced binary its
  # user-facing name. Quote the name because it intentionally contains spaces.
  set_target_properties(wwindows PROPERTIES OUTPUT_NAME "Wayland on X Windows")
  target_link_libraries(wwindows PRIVATE
      wwindows_core wwindows_input wwindows_x11 wwindows_i3
      wwindows_wayland wwindows_warnings)
endif()

if(WWINDOWS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
