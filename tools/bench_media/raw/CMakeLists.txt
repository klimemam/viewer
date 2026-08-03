cmake_minimum_required(VERSION 3.20)
project(rawprobe CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# LibRaw ships NO CMakeLists.txt. Its own README.cmake says cmake has not been
# officially supported since 2014 and points at a separate, unofficial repo
# (LibRaw/LibRaw-cmake). So FetchContent_MakeAvailable cannot work on it.
#
# Instead: compile its sources directly into our own target -- which is exactly
# what this repo already does for imgui (CMakeLists.txt:45-51) and miniz (:73).
# That makes this a test of "can LibRaw be consumed in THIS repo's idiom", not
# of "does LibRaw have good cmake support".
#
# SOURCE_SUBDIR does-not-exist is the same trick the repo uses for miniz/pfd to
# download without add_subdirectory (CMakeLists.txt:35, :42).
FetchContent_Declare(LibRaw
  GIT_REPOSITORY https://github.com/LibRaw/LibRaw.git
  GIT_TAG 0.22.2
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR does-not-exist
)
FetchContent_MakeAvailable(LibRaw)

file(GLOB_RECURSE LIBRAW_SRC
  ${libraw_SOURCE_DIR}/src/decoders/*.cpp
  ${libraw_SOURCE_DIR}/src/decompressors/*.cpp
  ${libraw_SOURCE_DIR}/src/demosaic/*.cpp
  ${libraw_SOURCE_DIR}/src/integration/*.cpp
  ${libraw_SOURCE_DIR}/src/metadata/*.cpp
  ${libraw_SOURCE_DIR}/src/postprocessing/*.cpp
  ${libraw_SOURCE_DIR}/src/preprocessing/*.cpp
  ${libraw_SOURCE_DIR}/src/tables/*.cpp
  ${libraw_SOURCE_DIR}/src/utils/*.cpp
  ${libraw_SOURCE_DIR}/src/write/*.cpp
  ${libraw_SOURCE_DIR}/src/x3f/*.cpp
  ${libraw_SOURCE_DIR}/src/libraw_c_api.cpp
  ${libraw_SOURCE_DIR}/src/libraw_datastream.cpp
)

# *_ph.cpp are LibRaw's PLACEHOLDER variants: they define the same symbols as
# the real demosaic/postprocessing files, and its own Makefile picks one set or
# the other, never both. A naive glob links both and fails with ~4 "multiple
# definition" errors (measured). Recording this because it is the concrete cost
# of LibRaw shipping no CMakeLists: whoever globs its sources has to know this,
# and has to re-learn it whenever upstream moves a file.
list(FILTER LIBRAW_SRC EXCLUDE REGEX "_ph\\.cpp$")

add_library(libraw_lib STATIC ${LIBRAW_SRC})
target_include_directories(libraw_lib PUBLIC ${libraw_SOURCE_DIR})
# LIBRAW_NODLL: static link on Windows. NOTHREADS keeps it to one TU-safe form.
# -w: third-party, do not police its warnings (same stance as miniz, :91).
target_compile_definitions(libraw_lib PUBLIC LIBRAW_NODLL)
target_compile_options(libraw_lib PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
# LibRaw calls ntohl/htons (byte-swapping in the metadata parsers), which on
# Windows live in winsock rather than libc. Measured: 12 undefined references
# without this. The viewer links no sockets today, so this WOULD be a new
# system library on Windows -- small, but worth naming rather than discovering.
if(WIN32)
  target_link_libraries(libraw_lib PUBLIC ws2_32)
endif()

add_executable(rawbench bench_raw.cpp)
target_link_libraries(rawbench PRIVATE libraw_lib)
