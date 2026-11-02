# Module test suites.
#
# Two tiers:
#   * portable suites -- no Wayland, no XCB, no X server. Always built.
#   * runtime suites  -- link the real core/input libraries and drive them
#                        against fakes. Built only with WWINDOWS_BUILD_RUNTIME,
#                        since they need libwayland-server to construct real
#                        wl_resource objects.
#
# Every suite links test_main.cpp, which discovers self-registered tests and
# fails if a suite registers none.

# test_main.cpp is compiled per-executable rather than linked from a shared
# library so each suite can be given its own name through WW_TEST_SUITE.
function(wwindows_add_test name suite)
  add_executable(${name} ${ARGN} ${CMAKE_CURRENT_SOURCE_DIR}/support/test_main.cpp)
  target_compile_features(${name} PRIVATE cxx_std_23)
  target_compile_definitions(${name} PRIVATE WW_TEST_SUITE="${suite}")
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE wwindows_warnings)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# --- portable suites --------------------------------------------------------
wwindows_add_test(wwindows_test_support support support/support_tests.cpp)
wwindows_add_test(wwindows_test_ports   ports   ports/ports_tests.cpp)

wwindows_add_test(wwindows_test_core core core/core_tests.cpp)
target_link_libraries(wwindows_test_core PRIVATE wwindows_core_portable)

# Policy suites compile the policy headers directly: the policies are
# header-only inline functions, and production code calls the same
# definitions these tests exercise.
wwindows_add_test(wwindows_test_input input input/input_tests.cpp)
wwindows_add_test(wwindows_test_x11   x11   x11/x11_tests.cpp)

# --- runtime suites ---------------------------------------------------------
if(WWINDOWS_BUILD_RUNTIME)
  # Wayland policy lives in a .cpp, so this suite needs the runtime tier.
  wwindows_add_test(wwindows_test_wayland wayland
      wayland/wayland_tests.cpp ../src/adapters/wayland/policy/scale.cpp)

  # Protocol globals, driven by a real libwayland client over a real socket.
  # The other Wayland suites construct wl_resources server-side, which skips
  # the request-unpacking half of these handlers entirely -- so this one
  # needs an actual client connection. Skipped (not built) without the
  # client development files.
  if(WAYLAND_CLIENT_FOUND)
    wwindows_add_test(wwindows_test_globals globals wayland/globals_tests.cpp)
    target_link_libraries(wwindows_test_globals PRIVATE
        wwindows_wayland wwindows_core wwindows_input
        PkgConfig::WAYLAND_CLIENT)
  else()
    message(STATUS
        "wayland-client not found -- protocol globals suite will not be built "
        "(install libwayland-dev to enable it)")
  endif()

  # Behavioral suites: the real state machines, driven against recording
  # fakes and a real in-process wl_display.
  wwindows_add_test(wwindows_test_surface_manager surface_manager
      core/surface_manager_tests.cpp)
  target_link_libraries(wwindows_test_surface_manager PRIVATE wwindows_core wwindows_wayland)

  wwindows_add_test(wwindows_test_input_translator input_translator
      input/input_translator_tests.cpp)
  target_link_libraries(wwindows_test_input_translator PRIVATE wwindows_input)

  # Keymap correctness is a separate suite because it pins XKB_DEFAULT_LAYOUT
  # per test, and it compiles the keymap the compositor sent using the client
  # half of xkbcommon -- neither of which belongs in the general input suite.
  wwindows_add_test(wwindows_test_xkb_keymap xkb_keymap
      input/xkb_keymap_tests.cpp)
  target_link_libraries(wwindows_test_xkb_keymap PRIVATE
      wwindows_input PkgConfig::XKBCOMMON)

  # i3 adapter: wire/JSON policy against literal input, and I3Watcher against
  # a fake i3 speaking the real framing over a real unix socket.
  wwindows_add_test(wwindows_test_i3 i3 i3/i3_tests.cpp)
  target_link_libraries(wwindows_test_i3 PRIVATE wwindows_i3)
  set_tests_properties(wwindows_test_i3 PROPERTIES TIMEOUT 30)

  # X11 integration: runs against a real (throwaway) X server, because an X11
  # adapter mocked at the XCB layer would mostly assert that it calls the
  # functions it calls. The wrapper provisions Xvfb and exits 77 (skip) when
  # it is unavailable.
  add_executable(wwindows_test_x11_backend
      x11/x11_backend_tests.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/support/test_main.cpp)
  target_compile_features(wwindows_test_x11_backend PRIVATE cxx_std_23)
  target_compile_definitions(wwindows_test_x11_backend PRIVATE
      WW_TEST_SUITE="x11_backend")
  target_include_directories(wwindows_test_x11_backend PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(wwindows_test_x11_backend PRIVATE
      wwindows_x11 wwindows_warnings)
  add_test(NAME wwindows_test_x11_backend
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/x11/run_with_xvfb.sh
                   $<TARGET_FILE:wwindows_test_x11_backend>)
  # RUN_SERIAL: run_with_xvfb.sh picks a free display number by checking
  # /tmp/.X11-unix and the lock file right before starting Xvfb -- a
  # check-then-act race with wwindows_test_transient_for_e2e below, the
  # only other test that also provisions its own throwaway Xvfb, if CTest
  # ever started both at the same instant under -j. Serializing the two
  # against each other (and incidentally against everything else, which
  # costs nothing real -- both finish in well under a second) is simpler
  # and actually correct, unlike shrinking the race window.
  set_tests_properties(wwindows_test_x11_backend PROPERTIES
      SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL TRUE)

  # xdg_toplevel.set_parent -> WM_TRANSIENT_FOR, end to end: a real
  # libwayland client through a real SurfaceManager through a real
  # X11Backend against the same throwaway Xvfb as wwindows_test_x11_backend
  # above. Needs wayland-client the same way wwindows_test_globals does, on
  # top of everything wwindows_test_x11_backend needs.
  if(WAYLAND_CLIENT_FOUND)
    add_executable(wwindows_test_transient_for_e2e
        x11/transient_for_e2e_tests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/support/test_main.cpp)
    target_compile_features(wwindows_test_transient_for_e2e PRIVATE cxx_std_23)
    target_compile_definitions(wwindows_test_transient_for_e2e PRIVATE
        WW_TEST_SUITE="transient_for_e2e")
    target_include_directories(wwindows_test_transient_for_e2e PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(wwindows_test_transient_for_e2e PRIVATE
        wwindows_x11 wwindows_wayland wwindows_core wwindows_input
        wwindows_warnings PkgConfig::WAYLAND_CLIENT)
    add_test(NAME wwindows_test_transient_for_e2e
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/x11/run_with_xvfb.sh
                     $<TARGET_FILE:wwindows_test_transient_for_e2e>)
    # RUN_SERIAL: see wwindows_test_x11_backend's own copy of this comment
    # above -- same throwaway-display race, same fix.
    set_tests_properties(wwindows_test_transient_for_e2e PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL TRUE)
  endif()

  # DRI3/Present zero-copy path. Needs a real GPU-backed X server, which
  # Xvfb is not, so this suite runs against the caller's own $DISPLAY and
  # skips (77) when that server has no DRI3 -- see x11/run_with_dri3.sh.
  #
  # GBM is what produces the real dmabufs the suite imports. It is an
  # optional dependency: without it the suite is simply not built, which is
  # honest, rather than being built against a synthesized fd that would
  # prove nothing about the import path.
  pkg_check_modules(GBM IMPORTED_TARGET gbm)
  if(GBM_FOUND)
    add_executable(wwindows_test_dri3
        x11/dri3_tests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/support/test_main.cpp)
    target_compile_features(wwindows_test_dri3 PRIVATE cxx_std_23)
    target_compile_definitions(wwindows_test_dri3 PRIVATE WW_TEST_SUITE="dri3")
    target_include_directories(wwindows_test_dri3 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(wwindows_test_dri3 PRIVATE
        wwindows_x11 wwindows_warnings PkgConfig::GBM)
    add_test(NAME wwindows_test_dri3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/x11/run_with_dri3.sh
                     $<TARGET_FILE:wwindows_test_dri3>)
    set_tests_properties(wwindows_test_dri3 PROPERTIES
        SKIP_RETURN_CODE 77 TIMEOUT 120)
  else()
    message(STATUS
        "gbm not found -- DRI3 zero-copy suite will not be built "
        "(install libgbm-dev to enable it)")
  endif()
endif()

# --- architecture -----------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  add_test(NAME wwindows_architecture_check
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/architecture/check_dependencies.py)
else()
  message(WARNING "python3 not found -- architecture check will not run")
endif()
