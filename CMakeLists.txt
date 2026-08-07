cmake_minimum_required(VERSION 3.21)
project(viewer C CXX)              # C: bundled plugins are pure C
if(WIN32)
  enable_language(RC)              # core/viewer.rc.in: exe icon + version block
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)   # use extraction time for FetchContent URL downloads
endif()

find_package(OpenGL REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)        # background sequence loader
include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip)
FetchContent_MakeAvailable(glfw)

# docking branch: same version, adds DockSpace so panels can be arranged freely
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8-docking.zip)
FetchContent_MakeAvailable(imgui)

# miniz: single-file deflate, used to read compressed .npz members
FetchContent_Declare(miniz
  URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
  SOURCE_SUBDIR does-not-exist)
FetchContent_MakeAvailable(miniz)

# portable-file-dialogs: header-only; post-0.1.0 snapshot needed for pfd::settings::available().
# Bogus SOURCE_SUBDIR makes MakeAvailable download without add_subdirectory.
FetchContent_Declare(pfd
  URL https://github.com/samhocevar/portable-file-dialogs/archive/c12ea8c9a727f5320a2b4570aee863bbede2a204.zip
  SOURCE_SUBDIR does-not-exist)
FetchContent_MakeAvailable(pfd)

# stb_image: the PNG/JPEG decoder behind core/imagefile.h. VENDORED as one
# header in third_party/stb/, deliberately breaking this project's own rule
# that no third-party source is committed.
#
# The rule is there so dependencies stay updatable and their provenance stays
# obvious, and fetching served both - until it was the ONLY thing a build
# needed the network for. glfw, imgui, miniz and pfd are all fetched too, but
# an existing checkout has them cached, so a new dependency is the only kind
# that surfaces: adding stb made a clean clone unbuildable offline, on the
# machine of the person who most needs to build one. That is a worse property
# than a committed file.
#
# Provenance is kept in the file, not in the build: stb_image.h carries its own
# version banner, THIRD-PARTY-NOTICES.md records the licence, and the commit
# below is the one it came from. It is MIT-or-public-domain, so committing it
# is permitted; the MIT option is the one taken.
#
#   https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h
#   SHA256 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
set(stb_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui_lib PUBLIC
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_lib PUBLIC glfw OpenGL::GL)
if(WIN32)
  # Without this, ImGui compiles its Win32 IME support OUT on non-MSVC compilers
  # (imgui.cpp: "MinGW users should either link with -limm32, or define
  # IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS"). The result is a MinGW build with
  # no ImmSetCompositionWindow/ImmSetCandidateWindow at all, so the Japanese IME
  # candidate window is never placed at the caret - it appears detached from the
  # field being typed into. Verified absent from the shipped binary's imports.
  target_compile_definitions(imgui_lib PUBLIC IMGUI_ENABLE_WIN32_DEFAULT_IME_FUNCTIONS)
  target_link_libraries(imgui_lib PUBLIC imm32)
endif()

# public plugin ABI header (include/ps/ps_plugin.h)
add_library(ps_api INTERFACE)
target_include_directories(ps_api INTERFACE ${CMAKE_SOURCE_DIR}/include)

# core/browse/*.cpp: Browse as real TUs since P7 (docs/split-plan.md §5 - the
# two files the plan budgets; instances.inc is #included by nav.cpp). The .inc
# fragments and headers stay off this list on purpose: the compiler depfile
# tracks them, so touching state.h or a fragment rebuilds exactly the TUs that
# include it.
add_executable(viewer core/main.cpp core/plugin_host.cpp core/ui_theme.cpp
                      core/serve.cpp core/remote.cpp core/app_icon.cpp
                      core/window_frame.cpp core/version.cpp core/adapter.cpp
                      core/imagefile.cpp core/stb_image_impl.c
                      core/browse/nav.cpp core/browse/panel.cpp
                      ${miniz_SOURCE_DIR}/miniz.c)
# The commit this binary was built from, refreshed on every build (a
# configure-time hash goes stale the moment you commit, and this string is
# written into exported data as provenance). version.cpp is its own tiny
# translation unit so a new commit relinks instead of recompiling main.cpp.
set(VIEWER_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/viewer_version_generated.h)
add_custom_target(viewer_version ALL
    COMMAND ${CMAKE_COMMAND} -DOUT=${VIEWER_VERSION_HEADER}
                             -DSRCDIR=${CMAKE_SOURCE_DIR}
                             -P ${CMAKE_SOURCE_DIR}/cmake/gitversion.cmake
    BYPRODUCTS ${VIEWER_VERSION_HEADER}
    COMMENT "resolving the build's commit")
add_dependencies(viewer viewer_version)
target_include_directories(viewer PRIVATE ${CMAKE_BINARY_DIR}/generated)
# third-party: do not police its warnings. MINIZ_NO_STDIO drops the archive
# file-I/O layer we never use (only the in-memory codec is called), which also
# silences its large-file #pragma message on MinGW.
set_source_files_properties(${miniz_SOURCE_DIR}/miniz.c PROPERTIES
                            COMPILE_OPTIONS "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>"
                            COMPILE_DEFINITIONS "MINIZ_NO_STDIO")
# ...and the same for stb_image's implementation, for the same reason. The
# wrapper around it (core/imagefile.cpp) is OURS and stays under -Wall -Wextra.
set_source_files_properties(core/stb_image_impl.c PROPERTIES
                            COMPILE_OPTIONS "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>;$<$<C_COMPILER_ID:MSVC>:/w>")
target_link_libraries(viewer PRIVATE imgui_lib ps_api Threads::Threads)
# where the server pulls its own peer binary from (binaries branch)
set(VIEWER_REPO_URL "https://github.com/klimemam/viewer.git" CACHE STRING
    "repository the remote peer bootstraps itself from")
target_compile_definitions(viewer PRIVATE VIEWER_REPO_URL="${VIEWER_REPO_URL}")
target_include_directories(viewer PRIVATE ${pfd_SOURCE_DIR} ${miniz_SOURCE_DIR} ${stb_SOURCE_DIR})
if(UNIX)
  target_link_libraries(viewer PRIVATE ${CMAKE_DL_LIBS})   # dlopen
endif()

# ---- application icon --------------------------------------------------------
# The mark is drawn by core/app_icon.cpp, so the repository carries a
# description instead of a folder of bitmaps. A host tool renders it into the
# formats each desktop wants; the window icon itself is set at runtime from the
# same code (and retinted when a remote session is up).
set(VIEWER_ICON_DIR ${CMAKE_BINARY_DIR}/icons)
if(CMAKE_CROSSCOMPILING)
  # a host tool cannot be run from a cross build; the exe just keeps the default
  # shell icon there, and the window icon still works (it is drawn at runtime)
  message(STATUS "cross-compiling: skipping the generated app icon files")
else()
  add_executable(mkicon tools/mkicon.cpp core/app_icon.cpp ${miniz_SOURCE_DIR}/miniz.c)
  target_include_directories(mkicon PRIVATE ${miniz_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/core)
  add_custom_command(
    OUTPUT ${VIEWER_ICON_DIR}/viewer.ico ${VIEWER_ICON_DIR}/viewer.png
           ${VIEWER_ICON_DIR}/viewer-remote.ico ${VIEWER_ICON_DIR}/viewer-remote.png
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VIEWER_ICON_DIR}
    COMMAND mkicon ${VIEWER_ICON_DIR}
    DEPENDS mkicon
    COMMENT "generating the application icon (ico + png)")
  add_custom_target(viewer_icons
                    DEPENDS ${VIEWER_ICON_DIR}/viewer.ico ${VIEWER_ICON_DIR}/viewer.png
                            ${VIEWER_ICON_DIR}/viewer-remote.ico
                            ${VIEWER_ICON_DIR}/viewer-remote.png)
  add_dependencies(viewer viewer_icons)
  if(WIN32)
    set(VIEWER_ICO "${VIEWER_ICON_DIR}/viewer.ico")
    configure_file(core/viewer.rc.in ${CMAKE_BINARY_DIR}/viewer.rc @ONLY)
    target_sources(viewer PRIVATE ${CMAKE_BINARY_DIR}/viewer.rc)
    set_source_files_properties(${CMAKE_BINARY_DIR}/viewer.rc PROPERTIES
                                OBJECT_DEPENDS "${VIEWER_ICO}")
  endif()
endif()

# The remote peer as its own binary. A compute server typically has no OpenGL and
# no X11, so a GUI binary would fail to load there before it could reach --serve.
# It links miniz, the C++ runtime - and plugin_host, so MEASURE runs the SAME
# analyzer plugins the local host runs (plugin_host is verified GL-free).
add_executable(viewer-serve core/serve_main.cpp core/serve.cpp core/plugin_host.cpp
                            ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(viewer-serve PRIVATE ${miniz_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/core)
target_link_libraries(viewer-serve PRIVATE ps_api)
if(UNIX)
  target_link_libraries(viewer-serve PRIVATE ${CMAKE_DL_LIBS})   # dlopen for plugins
endif()
if(MSVC)
  target_compile_options(viewer-serve PRIVATE /W3 /utf-8)
else()
  target_compile_options(viewer-serve PRIVATE -Wall -Wextra)
endif()
if(WIN32)
  # dwmapi: the integrated window frame asks DWM for the drop shadow and the
  # Windows 11 rounded corners a borderless window would otherwise lose
  target_link_libraries(viewer PRIVATE comdlg32 ole32 shell32 dwmapi)
endif()
# The integrated frame hands the drag and the resize back to the window manager
# through _NET_WM_MOVERESIZE, which needs the X11 handle GLFW is holding. A
# Wayland-only GLFW has no such handle (and Wayland has no such protocol for
# clients), so the frame stays a system frame there.
if(UNIX AND NOT APPLE AND GLFW_BUILD_X11)
  find_package(X11)                 # GLFW links it privately; this needs XSendEvent
  if(X11_FOUND)
    target_compile_definitions(viewer PRIVATE VIEWER_X11)
    target_link_libraries(viewer PRIVATE X11::X11)
  else()
    message(STATUS "no X11 development files: the integrated window frame will "
                   "fall back to moving the window by hand")
  endif()
endif()
if(MSVC)
  target_compile_options(viewer PRIVATE /W3 /utf-8)
else()
  target_compile_options(viewer PRIVATE -Wall -Wextra)
endif()
if(MINGW)   # self-contained exes: no libstdc++/libgcc/libwinpthread DLLs alongside
  foreach(tgt viewer viewer-serve)
    # -static, NOT --whole-archive on libwinpthread. Both make the binary
    # self-contained; only one of them leaves it able to say what it is.
    #
    # --whole-archive pulls in EVERY object of libwinpthread.a, including its
    # version.o - a VERSIONINFO resource for libwinpthread-1.dll. Two version
    # resources with the same id and language collide, the linker prints
    # ".rsrc merge failure: duplicate leaf: type: 10 (VERSION)" and keeps one,
    # and the one it kept was NOT ours. viewer.exe told Windows it was "POSIX
    # WinThreads for Windows", original filename libwinpthread-1.dll, by
    # MinGW-W64 Project - in Explorer's details pane, in the taskbar tooltip,
    # in every "what is this program" surface. That is worse than the bare
    # filename core/viewer.rc.in exists to replace, and it shipped.
    #
    # Dropping --whole-archive alone fixes the identity and reintroduces a
    # runtime dependency on libwinpthread-1.dll, which is the thing it was
    # there to prevent. -static gives both: verified, on both binaries, that
    # no libgcc/libstdc++/libwinpthread DLL is imported and that the version
    # block is viewer's own.
    #
    # --no-insert-timestamp: mingw stamps the PE header with the link time, so
    # building the same tree twice produced two different binaries. The export
    # provenance line names the commit this build came from; without this, that
    # commit does not name one binary, which is the whole point of writing it.
    target_link_options(${tgt} PRIVATE -static -Wl,--no-insert-timestamp)
  endforeach()
endif()

# ---- bundled plugins (pure C, MODULE libs into build/plugins) ----
set(VIEWER_PLUGIN_DIR ${CMAKE_BINARY_DIR}/plugins)
function(viewer_add_plugin name)
  add_library(${name} MODULE ${ARGN})
  target_link_libraries(${name} PRIVATE ps_api)
  set_target_properties(${name} PROPERTIES
    PREFIX ""                                        # no "lib" prefix on Unix
    LIBRARY_OUTPUT_DIRECTORY "$<1:${VIEWER_PLUGIN_DIR}>"  # genex kills per-config subdir
    C_VISIBILITY_PRESET hidden)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W3)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(${name} PRIVATE m)         # isfinite/sqrt
  endif()
endfunction()

viewer_add_plugin(display_falsecolor  plugins/display_falsecolor.c)
viewer_add_plugin(analyzer_stats      plugins/analyzer_stats.c)
viewer_add_plugin(analyzer_noise      plugins/analyzer_noise.c)
viewer_add_plugin(analyzer_prnu       plugins/analyzer_prnu.c)
viewer_add_plugin(analyzer_sharpness  plugins/analyzer_sharpness.c)
viewer_add_plugin(analyzer_esfr       plugins/analyzer_esfr.c)
viewer_add_plugin(proc_demosaic       plugins/proc_demosaic.c)

# The first test binary that is not `viewer` itself. boxblur2d is static inside
# analyzer_prnu.c and is pure arithmetic over a float buffer; reaching it through
# the viewer would mean driving a whole PRNU analysis to assert on a low-pass
# plane, so it gets a direct C unit test that #includes the plugin source.
add_executable(test_prnu plugins/test/test_prnu.c)
target_link_libraries(test_prnu PRIVATE ps_api)
if(UNIX AND NOT APPLE)
  target_link_libraries(test_prnu PRIVATE m)
endif()
# Held to the same standard as every plugin (viewer_add_plugin does this; a bare
# add_executable inherits none of it). It is clean today; this keeps it so.
if(MSVC)
  target_compile_options(test_prnu PRIVATE /W3)
else()
  target_compile_options(test_prnu PRIVATE -Wall -Wextra)
endif()

# The second, and it is built the same way for the same reason: fill_lut is
# static inside display_falsecolor.c, and the only way to reach it through the
# viewer is to register the plugin and drive a display. #including the source
# also keeps this off the shared-library name entirely - an earlier draft
# dlopen'd "plugins/display_falsecolor.so" by that literal name and was red on
# Windows, where the file is a .dll.
add_executable(test_display_falsecolor plugins/test/test_display_falsecolor.c)
target_link_libraries(test_display_falsecolor PRIVATE ps_api)
if(MSVC)
  target_compile_options(test_display_falsecolor PRIVATE /W3)
else()
  target_compile_options(test_display_falsecolor PRIVATE -Wall -Wextra)
endif()

# ---- selftests as ctest tests ---------------------------------------------
# The binary carries 25 --*-selftest flags. THIS BLOCK IS THE LIST: CI runs
# tools/run_selftests.sh, that script runs ctest, and a developer runs the same
# script to get exactly what CI got. There is no second copy of the argument
# lists anywhere - adding a test here adds it to CI.
#
# Fixtures come from tools/gen_testdata.py and are deterministic (fixed rng
# seeds). tools/testdata/ is gitignored: nothing binary is committed, and CI
# regenerates the whole tree on every run.
enable_testing()

# Every selftest NOT marked NOGL below builds a real GLFW window and an OpenGL
# context - VISIBLE, since GLFW_VISIBLE is only cleared for --bench and
# --browse-keys-selftest - so on a headless Linux box they need a display. Each
# test gets its own server via `xvfb-run -a` rather than sharing one, so a
# server that dies takes one test with it instead of the rest of the run.
# Six of the tests below do (seven counting the quarantined line at the bottom);
# the other 20 take the --no-window startup path and need neither a display nor
# this launcher.
set(VIEWER_TEST_LAUNCHER "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_program(XVFB_RUN xvfb-run)
  if(XVFB_RUN)
    set(VIEWER_TEST_LAUNCHER ${XVFB_RUN} -a "--server-args=-screen 0 1920x1200x24")
  else()
    message(STATUS "selftests: xvfb-run not found - the GUI selftests will need "
                   "a real DISPLAY on this machine")
  endif()
endif()

# Hermetic config dir. docs/verify-functional.md E-5: a scripted selftest that
# does not blank io.IniFilename writes the OPERATOR'S real
# %APPDATA%/viewer/layout.ini, and claimAutosaveSlot() drops a lock file there
# for every run. Pointing the config dir into the build tree keeps `ctest` from
# editing the developer's saved layout, and keeps a stale layout from deciding
# where a scripted click lands.
set(VIEWER_TEST_HOME ${CMAKE_BINARY_DIR}/selftest-home)
file(MAKE_DIRECTORY ${VIEWER_TEST_HOME})

# Relative fixture paths + WORKING_DIRECTORY at the source root, because
# --picker-selftest falls back to the literal "tools/testdata/batchset" when
# batchset is not a sibling of its own argument.
#
# Write NOGL as the first word to declare that a test needs no OpenGL context:
#
#     viewer_selftest(something NOGL --something-selftest tools/testdata/x)
#
# "Does this test need a GL context" is a PROPERTY OF THE TEST, carried here
# beside the test itself, and deliberately NOT a list kept somewhere else. The
# one word does BOTH halves of it: it labels the test `nogl` for
# tools/run_selftests.sh, and it passes --no-window to the binary, which is the
# startup path that makes no window and calls no GL. There is no second list to
# keep in step - in the harness, in the workflow, or in main.cpp - and a test
# moves between the two sets by gaining or losing this word.
#
# Get it wrong in the unsafe direction (NOGL on a test that draws) and the test
# stops at needWindow() in main.cpp, which names the test and this line. It
# cannot pass by accident.
function(viewer_selftest name)
  set(args ${ARGN})
  set(labels selftest)
  set(launcher ${VIEWER_TEST_LAUNCHER})
  set(extra_env "")
  if(args)
    list(GET args 0 first)
    if(first STREQUAL "NOGL")
      list(REMOVE_AT args 0)
      list(APPEND labels nogl)
      list(APPEND args --no-window)   # ...and take the startup path to match
      set(launcher "")           # needs no display, so it needs no xvfb either
    endif()
  endif()
  # ENV NAME=VALUE, repeatable, before the arguments: one more variable in the
  # test's environment. It exists for VIEWER_SERVE_LAG_MS, which the peer reads
  # (core/serve.cpp) - a property of the LINK a test runs over, which is not
  # something a command-line flag on this side can say.
  while(args)
    list(GET args 0 first)
    if(NOT first STREQUAL "ENV")
      break()
    endif()
    list(GET args 1 kv)
    list(APPEND extra_env "${kv}")
    list(REMOVE_AT args 0 1)
  endwhile()
  # A temp directory of its own, per test. The selftests write fixtures to FIXED
  # names under the system temp dir - viewer_seriesselftest.vsession,
  # viewer_hlenbomb, viewer_batchselftest and a dozen more - and APPDATA/HOME
  # alone do not separate those, because TMP and TEMP were inherited. So two
  # runs on one machine, in any two worktrees, shared one file: one process read
  # a one-stack session where it had written seven, and selftest.series failed
  # with "0 stack(s) open" while passing 20/20 alone. Reproduced at 2 failures
  # in 12 runs with three concurrent processes; the same shape sits under
  # selftest.browse (viewer_hlenbomb is remove_all'd and recreated by name), the
  # test whose assertion has now been lost three times.
  #
  # Fixing it here rather than at each of the sixteen call sites: the hazard is
  # "tests share a temp dir", and that is a property of how a test is launched.
  set(_tmp ${CMAKE_CURRENT_BINARY_DIR}/selftest-tmp/${name})
  file(MAKE_DIRECTORY ${_tmp})
  set(_env "APPDATA=${VIEWER_TEST_HOME};HOME=${VIEWER_TEST_HOME};TMP=${_tmp};TEMP=${_tmp};TMPDIR=${_tmp}")
  foreach(kv IN LISTS extra_env)
    list(APPEND _env "${kv}")
  endforeach()
  add_test(NAME selftest.${name}
           COMMAND ${launcher} $<TARGET_FILE:viewer> ${args})
  set_tests_properties(selftest.${name} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "${_env}"
    LABELS "${labels}"
    TIMEOUT 900)                 # the loaders' own budgets are 120-600 s
endfunction()

# ---- the capability gate ----------------------------------------------------
# Which of the tests below can run here is a question about the MACHINE, not
# about its OS name. `viewer --gl-probe` asks GLFW for exactly the context the
# real window asks for and prints either the renderer it got or, from GLFW's own
# error callback, why it got none. tools/run_selftests.sh runs this first: with
# a context every selftest runs (Linux under xvfb today, and Windows or macOS
# the day a runner gains one, with no change here); without one the `nogl` tests
# still run and the rest are skipped BY NAME, with the reason printed.
#
# It carries its own label rather than `selftest`, so it never counts as one of
# them - and it runs under the same ${VIEWER_TEST_LAUNCHER} they do, or it would
# be answering for a different environment than the one they are given.
add_test(NAME glprobe
         COMMAND ${VIEWER_TEST_LAUNCHER} $<TARGET_FILE:viewer> --gl-probe)
set_tests_properties(glprobe PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  ENVIRONMENT "APPDATA=${VIEWER_TEST_HOME};HOME=${VIEWER_TEST_HOME}"
  LABELS glprobe
  TIMEOUT 300)

# Not a --*-selftest flag on the viewer binary, so it is registered by hand -
# but it carries BOTH labels on purpose. run_selftests.sh picks its run set by
# label: `selftest` when the machine has a GL context, `nogl` when it does not.
# With `nogl` alone this test never ran on Linux CI (xvfb gives it a context, so
# CI takes the `selftest` branch) and never ran on any developer box with a
# working GL driver - and it appeared in neither the ran list nor the skipped
# list, which is the one thing that script promises cannot happen.
add_test(NAME test_prnu COMMAND test_prnu)
set_tests_properties(test_prnu PROPERTIES LABELS "selftest;nogl" TIMEOUT 60)

# Same hand registration, same two labels, and for the same reason - this one
# was written with `selftest` alone and would have repeated the bug above
# exactly, on the other branch: it would have run everywhere a GL context
# exists and nowhere else, silently, in neither list.
add_test(NAME test_display_falsecolor COMMAND test_display_falsecolor)
set_tests_properties(test_display_falsecolor PROPERTIES LABELS "selftest;nogl" TIMEOUT 60)

# Which of these carry NOGL was MEASURED, not reasoned about: every one was run
# through --no-window against this build, and the 20 below came back with output
# byte-identical to the same test with a window (the load-progress line
# "stack: loaded N frames" excepted - it is a snapshot of a race between the
# loader thread and the integrating pump, and it varies run to run in either
# mode). The five without it - browse-keys, browse-dbl, abstats, verify, tile
# - stop at needWindow() and say so; they drive real ImGui frames and
# assert on the LAYOUT that comes back, which is exactly what the backends the
# window carries are what produce.
#
# `roistats` is the one that had to EARN its NOGL rather than merely take it,
# because it is the only test here that draws ImGui frames without a backend
# under them (roiStatsSelftest() supplies the display size, the delta time and
# the rasterised font atlas by hand, which is all the two backends contribute
# that ImGui cannot proceed without). Measured the same way as the rest and to
# the same standard - `viewer --roistats-selftest` against
# `viewer --roistats-selftest --no-window`, byte-identical, no exceptions, which
# is why its dump prints numbers and no rectangles.
viewer_selftest(series      NOGL --series-selftest tools/testdata/linset)
# The Series Analysis panel's contract (docs/analysis-layers.md §3.3, 判断
# record 7): frame|stack mixed members, per-member folds with the sum refusal,
# the migrated fit window/method, and the session round trip. Values and
# strings only, so NOGL; it writes its own exact fixtures under a scratch
# subdirectory, the stackavg discipline.
viewer_selftest(seriespanel NOGL --seriespanel-selftest tools/testdata)
viewer_selftest(lin         NOGL --lin-selftest --stack always
                            --cfa bayer --bayer-pattern RGGB tools/testdata/linset)
viewer_selftest(browse      NOGL --browse-selftest tools/testdata/rb)
# Drives the real main loop, injecting keys and mouse frame by frame.
viewer_selftest(browse-keys --browse-keys-selftest tools/testdata/rb)
# ...and the same loop over a link that is SLOW. Every gesture browse-keys makes
# runs against a peer on this disk, so click one of a double-click costs nothing
# there and the panel's dependence on how long click one takes is invisible.
# Here meta and tile are held back 400 ms each - past ImGui's 0.30 s
# double-click window, which is the whole point - so click one blocks the UI
# thread the way a real ssh peer does. Asserted, in order:
#   1. a mouse-first double-click on a FILE opens it, even though click one
#      spent longer fetching the preview than the double-click window allows
#      (chknames without [pv] is "registered, not preview");
#   2. two clicks a SECOND apart on another file are still two clicks - the
#      credit for blocked time cannot manufacture a double-click out of them
#      (flat.npy stays [pv]);
#   3. the navigation latch is untouched: a folder's double-click enters once
#      and opens NOTHING (chkimgmark), which is the property CI caught the last
#      two attempts on.
#
# The aim is SHORT and relative: the arrows walk the cursor down the folder
# rows - which the keyboard does not preview, so the file below is still cold -
# and each gesture is then one row off the cursor. It used to be "home" plus an
# absolute offset, which has two problems: the offset silently means a
# different row whenever rb/ gains an entry, and after a click the cursor has
# MOVED to the row that was clicked, so the second half of "two clicks a second
# apart" was aiming N rows below its own first click and landing past the end
# of the listing. `click` is the same pixel by construction, which is what that
# claim needs.
viewer_selftest(browse-dbl ENV VIEWER_SERVE_LAG_MS=400
                --browse-keys-selftest tools/testdata/rb
                --browse-keys "waitdir:rb,viewreset,w400,home,chkatrow:..,\
down,down,down,down,down,down,chkatrow:unpadded,\
dbloff:1,chkimg:1,chknames:dark.npy,chkatrow:dark.npy,\
clickoff:1,idle:80,click,chkatrow:flat.npy,chkimg:2,\
chknames:dark.npy+flat.npy[pv],\
home,imgmark,dbloff:1,waitdir:digitset,chkimgmark,chkimg:2")
# The T group lays out the Temporal panel in real ImGui frames (the chart that
# was drawn past the panel's bottom edge is a layout fact, invisible to the
# recompute functions).
viewer_selftest(abstats     --abstats-selftest tools/testdata/multi)
viewer_selftest(range       NOGL --range-selftest tools/testdata/multi)
viewer_selftest(close       NOGL --close-selftest tools/testdata/multi)
viewer_selftest(batch       NOGL --batch-selftest tools/testdata/multi)
viewer_selftest(picker      NOGL --picker-selftest tools/testdata/abset)
# The ROI row probe: real frames with an injected cursor, asserting on where the
# rows the panel laid out actually are - the delete button's rect and the clicks
# that have to reach it past a span-all-columns Selectable.
viewer_selftest(verify      --verify-selftest tools/testdata/multi)
# ...and the same panel's NUMBERS, which are not a rectangle and so do not need
# a display. V19 used to hold both, which put the PRNU column's arithmetic
# behind a GL context and therefore on one runner of the three: it was measured
# on Linux and shipped everywhere. Same panel, same cache, same clipper, same
# PRNU cell, same probe - a frame with no backend under it. Analytic fixtures
# built in memory, so it needs no fixture directory.
viewer_selftest(roistats    NOGL --roistats-selftest)
# A and B (and a letter past B) on ONE document: the Statistics panel must draw
# BOTH sides, coinciding, rather than fold them into one. Same NOGL reasoning as
# roistats - it asserts what the panel prints, not where it landed - and the
# same analytic in-memory fixtures, so it needs no fixture directory either.
viewer_selftest(abeq        NOGL --abeq-selftest)
# Which dll computed the Analysis grid (#46 stage 1): the host ledger's file
# name must reach the provenance line and the Copy-table TSV, and builtin
# statistics must never wear a plugin tag. Same NOGL reasoning as roistats -
# strings, not layout - and the same analytic in-memory fixture; the analyzers
# themselves are the real dlls the build put in build/plugins.
viewer_selftest(anaprov     NOGL --anaprov-selftest)
viewer_selftest(scan        NOGL --scan-selftest tools/testdata/linset)
viewer_selftest(localbrowse NOGL --localbrowse-selftest tools/testdata/rb)
viewer_selftest(rtemporal   NOGL --rtemporal-selftest tools/testdata/rb/scanroot/10lx)
# Windowless despite touching ImGui: its one call is GetClipboardText, and with
# no backend installed ImGui's own default clipboard handler answers it (the
# Win32 clipboard on Windows, an in-process buffer elsewhere). Verified: the TSV
# it prints is identical with and without a window.
viewer_selftest(framestats  NOGL --stack always
                            --framestats-selftest tools/testdata/linset/80lx)
viewer_selftest(export      NOGL --export-selftest tools/testdata/multi --stack always)
# T12 lays the canvas out for real to read back the pane rectangles and badges.
viewer_selftest(tile        --tile-selftest tools/testdata/multi)
viewer_selftest(derive      NOGL --derive-selftest tools/testdata)
# The frame average is arithmetic over pixels, so none of it needs a context:
# it writes its own fixtures (exact means, NaN, a mosaic, a float32-defeating
# sum) under a scratch subdirectory and asserts the VALUES, the way
# --roistats-selftest does rather than the way a menu-item test would.
viewer_selftest(stackavg    NOGL --stackavg-selftest tools/testdata)
viewer_selftest(export-tsv  NOGL --export-tsv-selftest tools/testdata/multi --stack always
                            --cfa bayer --bayer-pattern RGGB)
# (frame-lin retired with the §3.3 migration: the frame-axis linearity fit
# lives in the Series Analysis panel now, and selftest.seriespanel above
# carries its fixture discipline - window inclusion/exclusion, per-plane auto
# windows, the two methods' predicted disagreement, NaN counting, the ROI
# convention - at the series layer, where the analysis is.)
viewer_selftest(newwin      NOGL --newwin-selftest tools/testdata)
# The 22nd flag, which the scratchpad orchestrator never ran: its fixture
# (levelfiles/) was hand-made and lived outside the repo. gen_testdata.py
# writes it now, so a sweep laid out as ONE .npy per level is covered in CI.
viewer_selftest(sweepfile   NOGL --sweepfile-selftest tools/testdata/levelfiles)

# The 23rd: which document holds which pixels (docs/verify-functional.md
# D-1/D-2/D-7). multi/ because it is three stacks of five frames - enough
# documents for "N documents, N sources" to say something - and its parent
# carries the .raw fixture the second decode path needs. NOGL: it dumps a
# table and draws nothing, verified by running it both ways.
viewer_selftest(srcmap NOGL --srcmap-selftest tools/testdata/multi)

# The 24th: PNG and JPEG through core/imagefile.h, and TIFF's refusal.
# Deliberately NOGL - what it asserts is bit depth, channel count, the note the
# Inspector prints and the wording of a refusal, none of which is a rectangle,
# so it gates on all three runners rather than on the one with a GL context.
viewer_selftest(media NOGL --media-selftest tools/testdata/media)

# The reload walk (docs/reference-design.md §3.2): a frame's FILE rewritten
# under a live stack. R0-R23 cover the invalidation walk, the temporal key
# (stackRev), the registry re-key, the recorded npy READING replayed on
# reload (R18a-c), and two readings of one file staying two tuples (R21b).
# It writes its own fixtures under a scratch subdirectory of the given dir.
viewer_selftest(reload NOGL --reload-selftest tools/testdata)

# --remote-selftest returns before startup reaches GLFW at all, so it was the
# one test carrying NOGL before there was a windowless startup path - and it
# must be told where the standalone peer is, or it silently tests this binary
# against itself instead of viewer-serve.
viewer_selftest(remote NOGL --remote-selftest tools/testdata/bench_stack.npy
                            --remote-exe $<TARGET_FILE:viewer-serve>)

# ---- QUARANTINE -----------------------------------------------------------
# Named, reasoned, and reported - never deleted and never loosened.
#
# docs/tasks.csv:73  "abstats A2既知失敗 / --cfa bayer併用時のB σ_t 1e-6不一致
# (スイート外) / 原因未調査".  The A2 assert compares the B slot's cached
# sigma_t against an independent recompute at 1e-6 relative; with --cfa bayer
# it was recorded as mismatching. It is NOT reproducible on this build: 10 runs
# out of 10 pass on MinGW against merged main, with both the generated and the
# original hand-made multi/ fixture. So it is either already fixed by another
# change or it is intermittent, and nobody has established which. Until someone
# does, it stays out of the gate rather than being allowed to redden the matrix
# at random. tools/run_selftests.sh prints this quarantine on every run.
# The argument ORDER matters and was wrong here: --abstats-selftest consumes the
# next token unconditionally (main.cpp next()), so "--cfa" became the selftest's
# DIRECTORY and the CFA was never applied. The quarantined test was a strictly
# weaker copy of selftest.abstats - identical A2 numbers, and 12 asserts skipped
# because sibling fixtures resolved against "." - which is why it "passed 10/10".
# Corrected, it fails 20/20, so it stays disabled until the defect it names is
# fixed; the point of correcting it is that lifting the quarantine will now test
# what the name says.
viewer_selftest(abstats-cfa-bayer --abstats-selftest tools/testdata/multi --cfa bayer)
set_tests_properties(selftest.abstats-cfa-bayer PROPERTIES
  DISABLED TRUE
  LABELS "selftest;quarantine")
