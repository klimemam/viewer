cmake_minimum_required(VERSION 3.21)
project(pixelscope_native C CXX)   # C: bundled plugins are pure C
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
                      ${miniz_SOURCE_DIR}/miniz.c)
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
    BYPRODUCTS ${VIEWER_ICON_DIR}/viewer-remote.png
    COMMAND ${CMAKE_COMMAND} -E make_directory ${VIEWER_ICON_DIR}
    COMMAND mkicon ${VIEWER_ICON_DIR}
    DEPENDS mkicon
    COMMENT "generating the application icon (ico + png)")
  add_custom_target(viewer_icons
                    DEPENDS ${VIEWER_ICON_DIR}/viewer.ico ${VIEWER_ICON_DIR}/viewer.png)
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
  target_link_libraries(viewer PRIVATE comdlg32 ole32 shell32)
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
