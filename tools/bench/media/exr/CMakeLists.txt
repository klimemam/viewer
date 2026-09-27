cmake_minimum_required(VERSION 3.20)
project(exrprobe CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Exactly what strategy B costs: official OpenEXR via FetchContent, pinned.
# OpenEXR's own CMake pulls Imath when find_package(Imath) fails, and 3.4
# vendors libdeflate. We do NOT pre-provide Imath, so the measured number
# includes whatever OpenEXR decides it needs.
set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(OpenEXR
  GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openexr.git
  GIT_TAG v3.4.13
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(OpenEXR)

add_executable(exrbench bench_exr.cpp)
target_link_libraries(exrbench PRIVATE OpenEXR::OpenEXR)

add_executable(exrgen gen_exr.cpp)
target_link_libraries(exrgen PRIVATE OpenEXR::OpenEXR)
