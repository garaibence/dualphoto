cmake_minimum_required(VERSION 3.20)
project(dualphoto VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---- options ---------------------------------------------------------------
option(DUALPHOTO_BUILD_APPS   "Build command-line apps"            ON)
option(DUALPHOTO_BUILD_TESTS  "Build tests and fake devices"       ON)
option(DUALPHOTO_WITH_X11     "Display backend: X11 (Xlib + SHM)"  ON)
option(DUALPHOTO_WITH_SDL     "Display backend: SDL2 (Wayland/X11)" ON)
option(DUALPHOTO_WITH_DRM     "Display backend: DRM/KMS (TTY only)" OFF)
option(DUALPHOTO_WITH_OPENCV  "Camera backend: OpenCV VideoCapture" ON)
option(DUALPHOTO_WITH_V4L2    "Camera backend: raw V4L2"            OFF)
option(DUALPHOTO_WITH_GPHOTO2 "Camera backend: gphoto2 (DSLRs)"     OFF)

# ---- shared warning flags ----------------------------------------------------
add_library(dualphoto_warnings INTERFACE)
target_compile_options(dualphoto_warnings INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(DualphotoHelpers)

# ---- components ----------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(patterns)
add_subdirectory(backends)

if(DUALPHOTO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DUALPHOTO_BUILD_APPS)
  add_subdirectory(apps)
endif()

# ---- summary -------------------------------------------------------------------
message(STATUS "")
message(STATUS "dualphoto ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  display    : X11=${DUALPHOTO_WITH_X11} SDL=${DUALPHOTO_WITH_SDL} DRM=${DUALPHOTO_WITH_DRM}")
message(STATUS "  camera     : OpenCV=${DUALPHOTO_WITH_OPENCV} V4L2=${DUALPHOTO_WITH_V4L2} gphoto2=${DUALPHOTO_WITH_GPHOTO2}")
message(STATUS "  apps/tests : ${DUALPHOTO_BUILD_APPS}/${DUALPHOTO_BUILD_TESTS}")
message(STATUS "")
