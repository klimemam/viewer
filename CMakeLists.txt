cmake_minimum_required(VERSION 3.21)
project(pixelscope_native CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)   # use extraction time for FetchContent URL downloads
endif()

find_package(OpenGL REQUIRED)
include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  URL https://github.com/glfw/glfw/archive/refs/tags/3.4.zip)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.zip)
FetchContent_MakeAvailable(imgui)

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

add_executable(viewer core/main.cpp)
target_link_libraries(viewer PRIVATE imgui_lib)
target_include_directories(viewer PRIVATE ${pfd_SOURCE_DIR})
if(WIN32)
  target_link_libraries(viewer PRIVATE comdlg32 ole32 shell32)
endif()
if(MSVC)
  target_compile_options(viewer PRIVATE /W3 /utf-8)
else()
  target_compile_options(viewer PRIVATE -Wall -Wextra)
endif()
