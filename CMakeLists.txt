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

add_executable(viewer core/main.cpp core/plugin_host.cpp core/ui_theme.cpp
                      core/serve.cpp core/remote.cpp core/app_icon.cpp
                      core/window_frame.cpp core/version.cpp
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
target_link_libraries(viewer PRIVATE imgui_lib ps_api Threads::Threads)
# where the server pulls its own peer binary from (binaries branch)
set(VIEWER_REPO_URL "https://github.com/klimemam/viewer.git" CACHE STRING
    "repository the remote peer bootstraps itself from")
target_compile_definitions(viewer PRIVATE VIEWER_REPO_URL="${VIEWER_REPO_URL}")
target_include_directories(viewer PRIVATE ${pfd_SOURCE_DIR} ${miniz_SOURCE_DIR})
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
    target_link_options(${tgt} PRIVATE -static-libgcc -static-libstdc++
                                       -Wl,-Bstatic,--whole-archive -lwinpthread
                                       -Wl,--no-whole-archive)
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

# ---- selftests as ctest tests ---------------------------------------------
# The binary carries 22 --*-selftest flags. THIS BLOCK IS THE LIST: CI runs
# tools/run_selftests.sh, that script runs ctest, and a developer runs the same
# script to get exactly what CI got. There is no second copy of the argument
# lists anywhere - adding a test here adds it to CI.
#
# Fixtures come from tools/gen_testdata.py and are deterministic (fixed rng
# seeds). tools/testdata/ is gitignored: nothing binary is committed, and CI
# regenerates the whole tree on every run.
enable_testing()

# Only --remote-selftest returns before glfwCreateWindow(). The other 21 build
# a real GLFW window and an OpenGL context - most of them VISIBLE, since
# GLFW_VISIBLE is only cleared for --bench and --browse-keys-selftest - so on a
# headless Linux box they need a display. Each test gets its own server via
# `xvfb-run -a` rather than sharing one, so a server that dies takes one test
# with it instead of the rest of the run.
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
function(viewer_selftest name)
  add_test(NAME selftest.${name}
           COMMAND ${VIEWER_TEST_LAUNCHER} $<TARGET_FILE:viewer> ${ARGN})
  set_tests_properties(selftest.${name} PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    ENVIRONMENT "APPDATA=${VIEWER_TEST_HOME};HOME=${VIEWER_TEST_HOME}"
    LABELS selftest
    TIMEOUT 900)                 # the loaders' own budgets are 120-600 s
endfunction()

viewer_selftest(series      --series-selftest tools/testdata/linset)
viewer_selftest(lin         --lin-selftest --sequence always
                            --cfa bayer --bayer-pattern RGGB tools/testdata/linset)
viewer_selftest(browse      --browse-selftest tools/testdata/rb)
viewer_selftest(browse-keys --browse-keys-selftest tools/testdata/rb)
viewer_selftest(abstats     --abstats-selftest tools/testdata/multi)
viewer_selftest(range       --range-selftest tools/testdata/multi)
viewer_selftest(close       --close-selftest tools/testdata/multi)
viewer_selftest(batch       --batch-selftest tools/testdata/multi)
viewer_selftest(picker      --picker-selftest tools/testdata/abset)
viewer_selftest(verify      --verify-selftest tools/testdata/multi)
viewer_selftest(scan        --scan-selftest tools/testdata/linset)
viewer_selftest(localbrowse --localbrowse-selftest tools/testdata/rb)
viewer_selftest(rtemporal   --rtemporal-selftest tools/testdata/rb/scanroot/10lx)
viewer_selftest(framestats  --sequence always
                            --framestats-selftest tools/testdata/linset/80lx)
viewer_selftest(export      --export-selftest tools/testdata/multi --sequence always)
viewer_selftest(tile        --tile-selftest tools/testdata/multi)
viewer_selftest(derive      --derive-selftest tools/testdata)
viewer_selftest(export-tsv  --export-tsv-selftest tools/testdata/multi --sequence always
                            --cfa bayer --bayer-pattern RGGB)
viewer_selftest(frame-lin   --frame-lin-selftest tools/testdata)
viewer_selftest(newwin      --newwin-selftest tools/testdata)
# The 22nd flag, which the scratchpad orchestrator never ran: its fixture
# (levelfiles/) was hand-made and lived outside the repo. gen_testdata.py
# writes it now, so a sweep laid out as ONE .npy per level is covered in CI.
viewer_selftest(sweepfile   --sweepfile-selftest tools/testdata/levelfiles)

# --remote-selftest is the one flag that returns before glfwInit(), so it needs
# no display - and it must be told where the standalone peer is, or it silently
# tests this binary against itself instead of viewer-serve.
add_test(NAME selftest.remote
         COMMAND $<TARGET_FILE:viewer> --remote-selftest tools/testdata/bench_stack.npy
                 --remote-exe $<TARGET_FILE:viewer-serve>)
set_tests_properties(selftest.remote PROPERTIES
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  ENVIRONMENT "APPDATA=${VIEWER_TEST_HOME};HOME=${VIEWER_TEST_HOME}"
  LABELS selftest
  TIMEOUT 900)

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
viewer_selftest(abstats-cfa-bayer --abstats-selftest --cfa bayer tools/testdata/multi)
set_tests_properties(selftest.abstats-cfa-bayer PROPERTIES
  DISABLED TRUE
  LABELS "selftest;quarantine")
