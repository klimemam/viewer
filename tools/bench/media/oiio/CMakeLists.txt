cmake_minimum_required(VERSION 3.20)
project(oiioprobe CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Strategy A probe: can OpenImageIO be brought in by FetchContent alone, on a
# machine with no vcpkg/conda/system packages? OIIO does not FetchContent its
# dependencies by default -- it find_package()s them. Since 2.5 it ships
# src/cmake/build_*.cmake recipes and OpenImageIO_BUILD_MISSING_DEPS, which is
# the ONLY way this can work here. That switch is what we are testing.
set(BUILD_SHARED_LIBS        OFF CACHE BOOL "" FORCE)
set(OpenImageIO_BUILD_MISSING_DEPS "all" CACHE STRING "" FORCE)
set(OpenImageIO_BUILD_LOCAL_DEPS   "all" CACHE STRING "" FORCE)
set(OIIO_BUILD_TOOLS         OFF CACHE BOOL "" FORCE)
set(OIIO_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING            OFF CACHE BOOL "" FORCE)
set(USE_PYTHON               OFF CACHE BOOL "" FORCE)
set(USE_QT                   OFF CACHE BOOL "" FORCE)
set(INSTALL_DOCS             OFF CACHE BOOL "" FORCE)
set(OIIO_INTERNALIZE_FMT     ON  CACHE BOOL "" FORCE)

# Minimise OIIO to the smallest thing that can still read an EXR. This makes
# the resulting build cost a LOWER BOUND on strategy A, not a fair picture of
# it: a real "OIIO for everything" build re-enables all of these.
# (The unminimised attempt died at libuhdr; see docs/media-formats.md 3.3.1.)
foreach(pkg libuhdr Ptex Freetype OpenJPEG OpenCV Libheif LibRaw WebP GIF
            DCMTK FFmpeg Nuke R3DSDK TBB TIFF PNG BZip2 Libsquish OpenVDB
            fmt Qt6 Qt5 Python3 pybind11)
  set(ENABLE_${pkg} OFF CACHE BOOL "" FORCE)
  set(USE_${pkg}    OFF CACHE BOOL "" FORCE)
endforeach()

FetchContent_Declare(OpenImageIO
  GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/OpenImageIO.git
  GIT_TAG v3.1.16.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(OpenImageIO)

add_executable(oiiobench bench_oiio.cpp)
target_link_libraries(oiiobench PRIVATE OpenImageIO::OpenImageIO)
