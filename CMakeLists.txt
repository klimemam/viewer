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

# TIFF has NO third-party line here, and that is the decision rather than an
# omission: core/tiffread.cpp is this project's own reader (see its header for
# why). libtiff cannot be vendored the way stb_image was - it is a configured,
# multi-file library - and fetching it would take away exactly the property the
# block above exists to protect, on the machine of the person who most needs an
# offline clean clone. So the only outside code TIFF uses is miniz, which was
# already fetched for .npz, and THIRD-PARTY-NOTICES.md gains nothing to record.

# ---- OpenEXR (official ASWF library) ---------------------------------------
# ...and .exr is where that same reasoning comes out the OTHER way, which is why
# both blocks are here to be read together.
#
# docs/media-support.md §1 studied this and recommended tinyexr; the user
# overruled that and chose the official library. That decision stands and the
# reason is the compression: a real .exr off a renderer is PIZ or DWA, and a
# hand-rolled reader that did "NONE and ZIP" would produce a stream of files
# that will not open. There is no small honest EXR reader the way there is a
# small honest TIFF reader - the refusals do not shrink the format here, they
# only remove the half of it that was worth having.
#
# So this is BY FAR the heaviest dependency in the tree, and the only one that
# is neither a single file nor a header. Its measured cost is recorded in
# docs/media-support.md §1 rather than left for someone to rediscover.
#
# Order of resolution, and why:
#   1. find_package(OpenEXR) - a distro/vcpkg/brew OpenEXR is used as-is. A
#      packager must never be made to download a second copy of a library the
#      system already ships.
#   2. FetchContent, pinned by URL (this repo's flavour: no git needed, the
#      archive is what the pin means).
# Imath is declared HERE rather than left to OpenEXR's own auto-fetch, which
# uses GIT_REPOSITORY - that would need git on the build machine and would not
# honour this repo's pinned-URL convention. OpenEXR checks `TARGET Imath::Imath`
# before fetching, so declaring it first wins.
#
# THERE IS NO OFF SWITCH, and that is the ruling of #53 (2026-08-09): 「既定ON
# で。OFFにするパスは不要です」. .exr is a format this viewer reads, not a
# feature a build elects, so the fetch below is UNCONDITIONAL.
#
# The honest consequence, stated here rather than discovered: an OFFLINE CLEAN
# CLONE CANNOT BUILD unless the machine already provides OpenEXR and Imath.
# There is no longer a flag that drops the dependency, so the fetch is the only
# other way to satisfy it. That is precisely the property the stb block above
# protects and the property TIFF's own reader was written to keep (core/
# tiffread.h), and it is the price the user accepted for reading real .exr.
# Two things soften it, and both are load-bearing:
#   1. find_package first (below). A machine with a system/vcpkg/brew OpenEXR
#      never touches the network - which matters MORE now than it did when the
#      flag existed, because it is the only escape that is left.
#   2. the standard FetchContent overrides, like the four above -
#        -DFETCHCONTENT_SOURCE_DIR_IMATH=<imath source tree>
#        -DFETCHCONTENT_SOURCE_DIR_OPENEXR=<openexr source tree>
#      point the build at trees that are already on the disk.
# Vendoring the sources, or caching them in CI, would remove it outright; that
# is a decision for the owner and not something this file may take on its own.
find_package(OpenEXR 3.1 QUIET)
if(OpenEXR_FOUND)
  message(STATUS "OpenEXR ${OpenEXR_VERSION}: using the installed copy (no download)")
else()
  message(STATUS "OpenEXR: not installed - fetching v3.4.13 + Imath v3.2.2 "
                 "(docs/media-support.md §1 carries the measured cost; "
                 "FETCHCONTENT_SOURCE_DIR_* points this at a local tree)")
  # Static, no install rules, no tools/examples/tests: we want the reader and
  # nothing else. BUILD_TESTING is CTest's own switch and this repo turns
  # enable_testing() on for its selftests, so it must be forced off for the
  # subprojects or their test suites join our ctest run.
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(IMATH_INSTALL OFF CACHE BOOL "" FORCE)
  set(IMATH_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
  set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
  set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
  set(OPENEXR_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
  set(OPENEXR_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(OPENEXR_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
  set(OPENEXR_TEST_LIBRARIES OFF CACHE BOOL "" FORCE)
  set(OPENEXR_TEST_TOOLS OFF CACHE BOOL "" FORCE)
  # 3.4 vendors libdeflate; forcing the internal one keeps the build
  # reproducible instead of depending on whatever the host happens to have.
  set(OPENEXR_FORCE_INTERNAL_DEFLATE ON CACHE BOOL "" FORCE)
  # EXCLUDE_FROM_ALL on FetchContent_Declare is 3.28+. Without it the default
  # target also builds OpenEXRUtil (deep/flat image helpers this viewer never
  # calls). Guarded rather than raising cmake_minimum_required from 3.21.
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(_exr_excl EXCLUDE_FROM_ALL)
  else()
    set(_exr_excl "")
  endif()
  FetchContent_Declare(Imath
    URL https://github.com/AcademySoftwareFoundation/Imath/archive/refs/tags/v3.2.2.zip
    ${_exr_excl})
  FetchContent_MakeAvailable(Imath)
  # OpenEXRSetup.cmake runs find_package(Imath CONFIG) BEFORE it checks
  # whether an Imath target already exists. With Imath declared here (at the
  # top level) that search lands on _deps/imath-build/config/ImathConfig.cmake,
  # which include()s an ImathTargets.cmake that is only written when
  # IMATH_INSTALL is ON - a hard configure error. OpenEXR guards against
  # exactly this with CMAKE_IGNORE_PATH, but it builds that path from ITS own
  # binary dir, so it only covers an Imath that OpenEXR itself fetched.
  # Suppressing the nested find is the honest fix: we have just supplied the
  # Imath::Imath target it is looking for, so it must not look.
  set(CMAKE_DISABLE_FIND_PACKAGE_Imath ON)
  FetchContent_Declare(openexr
    URL https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v3.4.13.zip
    ${_exr_excl})
  FetchContent_MakeAvailable(openexr)
  unset(CMAKE_DISABLE_FIND_PACKAGE_Imath)
  unset(_exr_excl)
endif()

# "third-party: do not police its warnings" (the stance miniz, stb and LibRaw
# take below) has a hole in it that only shows on a HEADER-heavy dependency:
# -w silences the third-party TU, and OpenEXR's problem is not in its TUs. Its
# headers are the interface - ImfHeader.h pulls ImfAttribute.h, which defines a
# TypedAttribute template whose readValueFrom(is, size, version) ignores two of
# its parameters - and a header is compiled with the flags of whoever INCLUDED
# it. So core/exrread.cpp and tools/mkexr.cpp, ours and under -Wall -Wextra,
# each reported three -Wunused-parameter against a file this project did not
# write and cannot fix: six warnings on macOS out of twelve, and none of them
# actionable by anyone reading them.
#
# It is only the FETCHED copy that does this. An installed OpenEXR arrives as
# an IMPORTED target, whose interface include dirs CMake already passes as
# -isystem; a FetchContent one is an ordinary target in this build tree, and
# ordinary targets are consumed with -I. Saying INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
# gives the fetched copy the same treatment the installed copy gets by default -
# so the two paths now warn identically, which is worth as much as the silence.
#
# Deliberately not blanket: this suppresses diagnostics inside THEIR headers.
# Our own code's warnings, including those raised by our USE of their API
# (wrong type, unused result, narrowing at our call site), are unaffected -
# -isystem stops at the header boundary.
foreach(_exr_tgt OpenEXR OpenEXRCore Iex IlmThread Imath)
  # ALIAS targets (OpenEXR::OpenEXR) cannot carry properties; the real ones can.
  # An installed OpenEXR only defines the aliases, so this loop finds nothing
  # and changes nothing - which is correct, it is already -isystem there.
  if(TARGET ${_exr_tgt})
    get_target_property(_exr_alias ${_exr_tgt} ALIASED_TARGET)
    get_target_property(_exr_inc ${_exr_tgt} INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _exr_alias AND _exr_inc)
      set_target_properties(${_exr_tgt} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_exr_inc}")
    endif()
  endif()
endforeach()
unset(_exr_tgt)
unset(_exr_alias)
unset(_exr_inc)

# ---- LibRaw (vendor RAW) -----------------------------------------------------
# The third block to read beside the two above, because it comes out a third
# way: the library is not optional, it has no CMake at all, and only a QUARTER
# of it is compiled.
#
# WHY A LIBRARY. Same rule as .exr, applied to a format that does not shrink
# when you refuse things: "vendor RAW" is thirty makers' compression schemes,
# and Canon's alone is an inverse wavelet transform. There is no small honest
# reader here the way there is for TIFF (core/rawread.h).
#
# WHY IT IS NOT FETCHED AS A CMAKE PROJECT. LibRaw ships no CMakeLists.txt;
# README.cmake says cmake has not been officially supported since 2014 and
# points at a separate, unofficial repository. So its sources are compiled
# straight into a target of ours - which is what this tree already does for
# imgui (above) and miniz - and SOURCE_SUBDIR does-not-exist suppresses the
# add_subdirectory the way the miniz and pfd blocks do.
#
# WHICH SOURCES, AND WHY IT IS THE INTERESTING PART. This does NOT compile
# src/demosaic, src/postprocessing, src/preprocessing or src/write. That is not
# a saving anyone invented here: it is upstream's own Makefile.devel.noppr2i
# variant, and the *_ph.cpp placeholders that replace them exist to make it
# buildable. What it buys is worth more than the seconds:
#
#   * dcraw_process(), dcraw_make_mem_image(), every demosaic and
#     subtract_black() are NOT IN THE BINARY. The rule that this viewer never
#     demosaics and never subtracts a declared black level stops being a rule
#     about which function we call and becomes a fact about what was linked.
#   * the DCB / FBDD demosaic code (third-party inside LibRaw, its own BSD-like
#     licence) is not distributed at all, so THIRD-PARTY-NOTICES.md has one
#     fewer thing to be right about.
#   * a naive glob of src/**.cpp does not link: the *_ph.cpp placeholders define
#     the SAME symbols as the real files (measured: 4 "multiple definition"
#     errors). Taking the placeholders and dropping their counterparts is the
#     other way out of that, and it is the one upstream supports.
#
# LIBRAW_NOTHREADS is deliberately NOT defined. It turns the bit-reader state
# in decoders_dcraw.cpp and sony_decrypt()'s pad into function-local statics
# shared by every thread; this viewer decodes a folder's frames on the sequence
# loader's thread while the UI thread can be opening another file, so the
# per-instance TLS form is the one that is correct here.
#
# OFFLINE: the standard override, like every other dependency here -
#     -DFETCHCONTENT_SOURCE_DIR_LIBRAW=<LibRaw source tree>
FetchContent_Declare(libraw
  URL https://github.com/LibRaw/LibRaw/archive/refs/tags/0.22.2.zip
  SOURCE_SUBDIR does-not-exist)
FetchContent_MakeAvailable(libraw)

file(GLOB_RECURSE LIBRAW_SRC ${libraw_SOURCE_DIR}/src/*.cpp)
set(LIBRAW_KEEP "")
foreach(f IN LISTS LIBRAW_SRC)
  if(f MATCHES "/src/demosaic/")
    continue()                                   # never called; never shipped
  elseif(f MATCHES "/src/libraw_c_api\.cpp$")
    continue()                                   # the C wrapper; this build is C++
  elseif(f MATCHES "/src/(postprocessing|preprocessing|write)/")
    if(NOT f MATCHES "_ph\.cpp$")
      continue()                                 # the placeholder is the one kept
    endif()
  endif()
  list(APPEND LIBRAW_KEEP ${f})
endforeach()
add_library(libraw_lib STATIC ${LIBRAW_KEEP})
target_include_directories(libraw_lib PUBLIC ${libraw_SOURCE_DIR})
target_compile_definitions(libraw_lib PUBLIC LIBRAW_NODLL)   # static, no dllimport
# third-party: do not police its warnings, the same stance miniz and stb take.
# core/rawread.cpp is OURS and stays under -Wall -Wextra.
target_compile_options(libraw_lib PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w> $<$<CXX_COMPILER_ID:MSVC>:/w>)
if(WIN32)
  # LibRaw's metadata parsers byte-swap with ntohl/htons, which on Windows live
  # in winsock rather than in libc: 12 undefined references without this. It is
  # the first socket library this project has ever linked, so it is named here
  # rather than left to be discovered from a link error.
  target_link_libraries(libraw_lib PUBLIC ws2_32)
endif()

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
                      core/imagefile.cpp core/tiffread.cpp core/exrread.cpp
                      core/rawread.cpp
                      core/y4mread.cpp core/stb_image_impl.c
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

# ---- no backend may build its decoder on the STACK --------------------------
# A format backend wraps a library, and a library's context can be enormous:
# sizeof(LibRaw) is 768,512 bytes, because imgdata.color alone carries a
# 65536-entry tone curve and a 4102-entry black level, and the constructor
# memsets all of it. `LibRaw R;` as a local was therefore a 770 KB stack frame -
# and Windows gives the MAIN thread one megabyte (SizeOfStackReserve 0x100000,
# what MSVC links and so what every shipped Windows build has) against Linux's
# and macOS's eight.
#
# What made that a bug worth a gate rather than a tidy-up is that it did not
# fail HERE. It failed as `selftest.media ***Exception: SegFault` on
# windows-latest, on two branches that touched nothing near this path, while
# main stayed green: whether 770 KB fit in what was left of the megabyte
# depended on the caller's frame - `main`, which #includes every selftest body -
# so the crash was a property of the binary's LAYOUT and any unrelated change
# could flip it. Nothing in the source said which line was responsible.
#
# -Wframe-larger-than= is what says it. 64 KB against a measured worst case of
# under 4 KB across all five backends at -O0, -O2 and -O3, so it has sixteen
# times the headroom it needs and cannot misfire on an ordinary edit; the one
# thing it catches is a library context that landed on the stack. It is an
# ERROR, on the two platforms whose compilers can measure it - which are exactly
# the two that never see the crash, and can now refuse to build the cause of it.
# MSVC has no equivalent, so Windows keeps gating the way it always did: by
# running the tests.
#
# ...and it was ONE platform, not two, from the hour it was written (7ec9a8b),
# because the two compilers do not spell it the same way and clang said so in a
# voice nobody was listening in. GCC takes the size on the -Werror= form:
#
#     -Werror=frame-larger-than=65536                     (gcc: gate, armed)
#
# clang parses -Werror=<name> and finds no warning called
# "frame-larger-than=65536"; its own is "frame-larger-than", and the SIZE is
# carried by a separate -W flag. So on macOS every build of these five files
# printed
#
#     warning: unknown warning option '-Werror=frame-larger-than=65536';
#              did you mean '-Werror=frame-larger-than='?
#
# five times - and an unknown -W is a WARNING to clang, never an error, so the
# gate was absent and the build was green. That is the failure this repo keeps
# writing down: a check that is present, passing, and answering nothing. macOS
# would have compiled `LibRaw R;` as a local without complaint.
#
# Two flags, then, and the size on the plain -W where clang wants it:
#
#     -Wframe-larger-than=65536 -Werror=frame-larger-than (clang: gate, armed)
#
# Verified by MEASUREMENT rather than by reading, at the -O3 a Release build
# uses, with a 100,000-byte local whose address escapes: gcc and clang each
# refuse to compile it under their own spelling and each reject the other's.
#
# -Werror=unknown-warning-option is the half that matters more than either. It
# is why this was invisible: clang shrugs at a flag it cannot parse, so ANY
# misspelled guard here silently stops guarding. Now a guard that cannot be
# spelled fails the build instead, and the next one to be added cannot repeat
# this. (GCC has no equivalent - it errors on an unknown -Werror= by itself.)
if(NOT MSVC)
  set_source_files_properties(
    core/imagefile.cpp core/rawread.cpp core/tiffread.cpp
    core/exrread.cpp core/y4mread.cpp
    PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:GNU>:-Werror=frame-larger-than=65536>;$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wframe-larger-than=65536;-Werror=frame-larger-than;-Werror=unknown-warning-option>")
endif()
target_link_libraries(viewer PRIVATE imgui_lib ps_api Threads::Threads libraw_lib)
# where the server pulls its own peer binary from (binaries branch)
set(VIEWER_REPO_URL "https://github.com/klimemam/viewer.git" CACHE STRING
    "repository the remote peer bootstraps itself from")
target_compile_definitions(viewer PRIVATE VIEWER_REPO_URL="${VIEWER_REPO_URL}")
target_include_directories(viewer PRIVATE ${pfd_SOURCE_DIR} ${miniz_SOURCE_DIR} ${stb_SOURCE_DIR})
# Only the GUI binary reads .exr. viewer-serve deliberately does NOT link it:
# the remote peer serves .npy only (docs/media-support.md §1, plan (a):
# local-only until the loaders move to a shared TU), and the peer is the binary
# that has to build on a bare compute server. TIFF held that line by living
# behind core/imagefile.h, which viewer-serve does not compile; OpenEXR holds it
# the same way, and the selftest evidence for both is the same one fact -
# viewer-serve.exe is byte-identical across the change.
target_link_libraries(viewer PRIVATE OpenEXR::OpenEXR)
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
# ...and version.cpp, since MOP_SET_FOLD's provenance trailer is the PEER's own
# build: a set analyzer is a built-in, so "which build computed this" is the
# viewer version and there is no dll to name instead (#46, docs/abi-v3.md §11).
add_executable(viewer-serve core/serve_main.cpp core/serve.cpp core/plugin_host.cpp
                            core/version.cpp
                            ${miniz_SOURCE_DIR}/miniz.c)
add_dependencies(viewer-serve viewer_version)
target_include_directories(viewer-serve PRIVATE ${miniz_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/core
                                                ${CMAKE_BINARY_DIR}/generated)
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

# ---- ABI compatibility fixtures (test-only plugins, NOT shipped) ----
# Two dlls carrying ONE measurement through the two ABI paths: registered as V2
# (the shape every bundled analyzer uses) and as V3 with a declared version and
# headline. They are built exactly like a shipped plugin - same warnings, same
# visibility, same MODULE - because a fixture built to easier rules proves
# nothing about what a user's plugin will do. They land in their own directory
# so the viewer's analyzer list is unchanged; --anaprov-selftest loads it by
# name, which is also how it can assert on both halves in one grid.
set(VIEWER_ABITEST_PLUGIN_DIR ${CMAKE_BINARY_DIR}/plugins-abitest)
function(viewer_add_test_plugin name)
  viewer_add_plugin(${name} ${ARGN})
  set_target_properties(${name} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "$<1:${VIEWER_ABITEST_PLUGIN_DIR}>")
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/plugins/test)
endfunction()
viewer_add_test_plugin(abi_v2_twin plugins/test/abi_v2_twin.c)
viewer_add_test_plugin(abi_v3_twin plugins/test/abi_v3_twin.c)
# ...and the third: a real STACK analyzer (ABI v3 §5), which is the only way to
# test a mouth whose whole content is what happens DURING a call - who serves a
# frame, how long it stays valid, what a NULL means. It carries three
# descriptors because the gate (§6) is proved by a body that would shout if it
# ever ran, and an out-of-range pull by one that must fail rather than invent.
viewer_add_test_plugin(abi_stack_twin plugins/test/abi_stack_twin.c)

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

# The third, and the only one whose subject is not arithmetic: the probe rule
# (docs/abi-v3.md §2.2) is a claim about hosts this repo does not contain - an
# ABI-1 host, an ABI-2 host, a v3 host with the seat still empty. The test
# builds those four hosts as structs and asks the real plugin what it does with
# each, which no test driven through plugin_host can do: that one can only ever
# present the single host this build happens to have.
add_executable(test_abi_probe plugins/test/test_abi_probe.c)
target_link_libraries(test_abi_probe PRIVATE ps_api)
target_include_directories(test_abi_probe PRIVATE ${CMAKE_SOURCE_DIR}/plugins/test)
if(MSVC)
  target_compile_options(test_abi_probe PRIVATE /W3)
else()
  target_compile_options(test_abi_probe PRIVATE -Wall -Wextra)
endif()

# ---- the .exr fixtures -------------------------------------------------------
# tools/gen_testdata.py writes every other media fixture from its own samples,
# with numpy and struct and zlib. It cannot write these: PIZ is a wavelet and
# DWAB is a DCT, and those two are the entire reason the official library is
# linked at all - coverage that stopped at NONE and ZIP would stop exactly where
# the argument starts.
#
# So the writer half of OpenEXR builds the fixtures, in a TOOL. Issue #53's
# judgment item 2 was that the original branch did this inside viewer.exe, which
# shipped OpenEXR's scanline, tiled and deep WRITERS to every user so that a
# test could run. Here the product binary links the reader only, and the same
# fixtures cost it nothing. Same pattern as `mkicon` above: a generated asset,
# produced at build time by a host tool, never committed.
#
# It writes into the source tree because tools/testdata IS the fixture tree
# (gitignored, regenerated, shared with gen_testdata.py, which creates its
# media/ with exist_ok and so leaves these alone). The stamp lives inside that
# tree so that deleting the tree regenerates them.
if(NOT CMAKE_CROSSCOMPILING)
  add_executable(mkexr tools/mkexr.cpp)
  target_link_libraries(mkexr PRIVATE OpenEXR::OpenEXR)
  if(MSVC)
    target_compile_options(mkexr PRIVATE /W3 /utf-8)
  else()
    target_compile_options(mkexr PRIVATE -Wall -Wextra)
  endif()
  set(VIEWER_EXR_FIXTURE_DIR ${CMAKE_SOURCE_DIR}/tools/testdata/media)
  add_custom_command(
    OUTPUT ${VIEWER_EXR_FIXTURE_DIR}/.exr-fixtures.stamp
    COMMAND mkexr ${VIEWER_EXR_FIXTURE_DIR}
    COMMAND ${CMAKE_COMMAND} -E touch ${VIEWER_EXR_FIXTURE_DIR}/.exr-fixtures.stamp
    DEPENDS mkexr
    COMMENT "writing the .exr fixtures for --media-selftest")
  add_custom_target(viewer_exr_fixtures ALL
                    DEPENDS ${VIEWER_EXR_FIXTURE_DIR}/.exr-fixtures.stamp)
else()
  # The one case left: a host tool cannot be RUN from a cross build, so the
  # fixtures cannot be written here. Never silent about it - the reader is
  # still linked and still read .exr, but M16-M23 have no files to open, so a
  # cross build is not a build these asserts have been run against.
  message(STATUS "cross-compiling: no .exr fixtures (a host tool cannot run here) - "
                 "--media-selftest's .exr asserts have nothing to open")
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
# Six of the tests below do; the other 36 take the --no-window startup path and
# need neither a display nor this launcher.
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

# ---- THE LABELLING RULE, for tests registered by hand -----------------------
# Everything above goes through viewer_selftest(), which gets this right by
# construction. A test registered with a bare add_test() does not, and both
# ways of getting it wrong have already happened once each, so the rule is
# written here rather than left to be re-derived:
#
#     `selftest` is MEMBERSHIP - it is what puts a test in the gate.
#     `nogl`     is a MODIFIER - it says the test also runs where there is no
#                GL context. It ACCOMPANIES `selftest`; it never replaces it.
#
# So a hand-registered test that needs no window carries LABELS "selftest;nogl"
# - both words - and one that draws carries "selftest" alone.
#
# WHY the asymmetry bites: tools/run_selftests.sh picks its run set by label,
# `selftest` where a GL context exists and `nogl` where none does, and it
# computes BOTH the ran list and the skipped list from `selftest`. A test
# labelled `nogl` alone is therefore in neither list in either branch: it does
# not run where there is a context (Linux CI under xvfb, and every developer
# box with a working driver), and where there is none it runs unannounced.
# That is precisely the one outcome that script exists to make impossible.
#
# It is no longer only a rule: run_selftests.sh refuses to run when a `nogl`
# test is not also a `selftest`, and names any registered test that is in no
# run set at all. Getting this wrong now stops the suite instead of emptying it.

# Not a --*-selftest flag on the viewer binary, so it is registered by hand -
# but it carries BOTH labels, per the rule above. With `nogl` alone (as it was
# until 1d951d3) this test never ran on Linux CI and never ran on any developer
# box with a working GL driver, and appeared in neither count.
add_test(NAME test_prnu COMMAND test_prnu)
set_tests_properties(test_prnu PROPERTIES LABELS "selftest;nogl" TIMEOUT 60)

# Same hand registration, same two labels, same rule. This one was written with
# `selftest` alone, which is the OTHER way to get it wrong: it would have run
# only where a GL context exists - Linux CI and nowhere else in the matrix -
# while reading, on Windows and macOS, as a test that needed a window.
add_test(NAME test_display_falsecolor COMMAND test_display_falsecolor)
set_tests_properties(test_display_falsecolor PROPERTIES LABELS "selftest;nogl" TIMEOUT 60)

# Third of the same kind, same two labels. It opens no file and needs no
# fixture: its whole subject is four psHostApi structs it fills in itself.
add_test(NAME test_abi_probe COMMAND test_abi_probe)
set_tests_properties(test_abi_probe PROPERTIES LABELS "selftest;nogl" TIMEOUT 60)

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
# AnalysisSet (docs/reader-analysisset.md): transport read, Ref binding order,
# close semantics, session block + adopt. Fixtures are built by the test
# itself under a scratch dir, so the argument is only a place to put them.
viewer_selftest(aset        NOGL --aset-selftest tools/testdata)
# The SetAnalyzers over a bound set (docs/flat-field-stats.md (b), #57 judgment
# 4): direct DSNU / PRNU, every refusal path, and the scan that keeps the bare
# famous names in one file. The argument is tools/testdata/multi, whose injected
# fixed pattern is SHARED between its three stacks and is therefore ground truth
# for both estimators; the exact-arithmetic fixtures are built in memory. The
# scan walks the sources relative to WORKING_DIRECTORY, which viewer_selftest
# already pins to the source root.
viewer_selftest(setanalysis NOGL --setanalysis-selftest tools/testdata/multi)
# The detrend AnalysisPreProcessor (docs/flat-field-stats.md 判断6/7): the raw
# default, the shading figure that is printed whether or not anything was
# removed, the product and the predicted direction its statistics move in, and
# the recipe a session brings it back by. The argument is only a place to write
# the fixture stack - it is synthetic, with a shading profile whose p-p and
# centre are known in closed form, so every expected number is arithmetic. NOGL
# for the roistats reasoning: what it asserts is numbers and strings.
viewer_selftest(detrend     NOGL --detrend-selftest tools/testdata)
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
# The stack mouth (ABI v3 §5) and the partial-load contract (§6). Same NOGL and
# same in-memory reasoning as anaprov, and it loads the same plugins-abitest
# directory: what a stack analyzer is handed, how long it may hold it, and the
# order in which "may this run at all" and "what is it being run on" are
# decided. Registered here rather than folded into anaprov because a failure
# should name the mouth it is about.
viewer_selftest(stackana    NOGL --stackana-selftest)
# The V3 migration of the five SHIPPED analyzers (docs/abi-v3.md §1). Same NOGL
# and same in-memory reasoning as anaprov, and it deliberately loads NO fixture
# directory: its subject is what a user actually gets from build/plugins, and a
# twin standing in for one of them would prove something about the twin. Its own
# test rather than a section of anaprov because its second claim - that no
# result moved - is asserted by DIFFING its stdout across two commits, and a
# failure of the first claim should not silence that output.
viewer_selftest(bundled     NOGL --bundled-selftest)
viewer_selftest(scan        NOGL --scan-selftest tools/testdata/linset)
viewer_selftest(localbrowse NOGL --localbrowse-selftest tools/testdata/rb)
# #111. Pointed at media/ and not at rb/ on purpose: rb/ is where the picture
# formats are absent, and it is also where --browse-keys addresses rows BY INDEX
# (tools/gen_testdata.py says so at length), so a .png added there would re-aim a
# dozen assertions that have nothing to do with this.
viewer_selftest(fmtgate     NOGL --fmtgate-selftest tools/testdata/media)
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
# under a live stack. R0-R24 cover the invalidation walk, the temporal key
# (stackRev - from the reload walk in R7/R8 and from a crop in R24), the
# registry re-key, the recorded npy READING replayed on reload (R18a-c), and
# two readings of one file staying two tuples (R21b).
# It writes its own fixtures under a scratch subdirectory of the given dir.
viewer_selftest(reload NOGL --reload-selftest tools/testdata)

# --remote-selftest returns before startup reaches GLFW at all, so it was the
# one test carrying NOGL before there was a windowless startup path - and it
# must be told where the standalone peer is, or it silently tests this binary
# against itself instead of viewer-serve.
viewer_selftest(remote NOGL --remote-selftest tools/testdata/bench_stack.npy
                            --remote-exe $<TARGET_FILE:viewer-serve>)

# Plugin analysis over the link (ABI v3 §10): name+version parity, the refusals
# that parity produces, and the stack mouth pulled frame by frame on the peer.
# Same NOGL and same standalone-peer reasoning as selftest.remote above.
#
# The two ENV lines are what make it a test rather than a demonstration.
# VIEWER_SERVE_PLUGINS points the PEER at the ABI fixture directory - a compute
# node's plugins are not beside its binary, and the fixtures are deliberately
# not shipped. VIEWER_SERVE_PIN_BUDGET_MB=1 is the arithmetic: bench_stack's
# frames are 0.3125 MB, so the eight the test asks for cannot be resident at
# once inside 1 MB, and the run only succeeds because the fixture releases each
# frame and the peer's window really recycles the slot (docs/abi-v3.md §9.2).
viewer_selftest(rplugin NOGL
                ENV VIEWER_SERVE_PLUGINS=${VIEWER_ABITEST_PLUGIN_DIR}
                ENV VIEWER_SERVE_PIN_BUDGET_MB=1
                --rplugin-selftest tools/testdata/bench_stack.npy
                --remote-exe $<TARGET_FILE:viewer-serve>)

# Set analysis folded on the peer (MOP_SET_FOLD, docs/analysis-layers.md §3.5).
# The same standalone-peer reasoning again - a client talking to itself cannot
# tell "the peer folded that" from "I did".
#
# It writes its own fixture under a scratch subdirectory of the given dir - the
# --stackavg / --derive precedent - because the shared trees have no injected
# fixed pattern and every row would clamp: "local 0 == peer 0" is an agreement
# about nothing, which is the exact trap #57 judgment 2 named.
#
# --cfa bayer is not decoration either. The plane map is one of the two things
# the local fold and the peer's must agree on sample for sample (the other is
# the frame order), and the two are written in different files against different
# copies of the table.
#
# It starts a SECOND peer under VIEWER_SERVE_PROTOCOL=7, which is how the
# too-old refusal is exercised in the gate rather than by keeping an old binary
# on a disk somewhere.
viewer_selftest(rset NOGL --rset-selftest tools/testdata
                --cfa bayer --bayer-pattern RGGB
                --remote-exe $<TARGET_FILE:viewer-serve>)

# The same A/B statistics run over a MOSAICED stack: --cfa bayer makes every
# 1-channel frame a Bayer frame, which is the case where the panels' rule "CFA
# planes are never mixed in a statistic" (docs/terminology.md) is load-bearing.
# It was quarantined (DISABLED) from 6308888 until the A2 mismatch it names was
# understood; it is understood now and the test is back in the gate.
#
# What it was: with --cfa bayer, A2's sigma_t comparison failed 20/20 at a
# relative 3.2e-3 against its 1e-6 tolerance - cached 25.0850055866 vs the
# selftest's independent recompute 25.0045948567. The reading recorded on the
# board had the two sides the wrong way round and concluded that temporal[1]'s
# cache key was missing the CFA setting. It is not: the key has carried cfa and
# cfaPattern since it was written (core/app/state.h TemporalState). The number
# that "exactly equals the CFA-off value" is the RECOMPUTE, not the cache -
# because the selftest's own refSigmaT pooled all four planes into one sigma_t
# and so measured the same thing with the mosaic on as with it off, while the
# panel published four per-plane numbers. 25.085 is the R plane, and it always
# was. The assert was never loosened: it is now made per plane, so the run
# checks four numbers where it used to check one.
#
# THE ARGUMENT ORDER BELOW IS LOAD-BEARING. --abstats-selftest consumes the
# next token unconditionally (core/app/cli.inc next()), so "--cfa" written
# immediately after it becomes the selftest's DIRECTORY and the mosaic is never
# applied to anything. That is what this line said until 6308888, and it is why
# the quarantined test once "passed 10/10" - it was a weaker copy of
# selftest.abstats with 12 asserts silently skipped against ".". --cfa goes
# AFTER the directory; the run prints "series=4" when it took effect.
viewer_selftest(abstats-cfa-bayer --abstats-selftest tools/testdata/multi --cfa bayer)
