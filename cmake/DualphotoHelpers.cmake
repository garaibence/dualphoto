# Helpers that keep the per-directory CMakeLists tiny and let the project
# configure cleanly while directories are still empty.

# dualphoto_add_module(<short_name>
#     [SRC_DIR <dir>]        # default: ./src
#     [INCLUDE_DIR <dir>]    # default: ./include if it exists, else .
#     [PUBLIC_LIBS ...]
#     [PRIVATE_LIBS ...])
#
# Creates target dualphoto_<short_name> with alias dualphoto::<short_name>.
# If no .cpp files exist yet it becomes an INTERFACE library so everything
# that links against it still configures.
function(dualphoto_add_module short_name)
  cmake_parse_arguments(ARG "" "SRC_DIR;INCLUDE_DIR" "PUBLIC_LIBS;PRIVATE_LIBS" ${ARGN})

  if(NOT ARG_SRC_DIR)
    set(ARG_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
  endif()
  if(NOT ARG_INCLUDE_DIR)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include")
      set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
    else()
      set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
  endif()

  set(tgt dualphoto_${short_name})
  file(GLOB_RECURSE srcs CONFIGURE_DEPENDS "${ARG_SRC_DIR}/*.cpp")

  if(srcs)
    add_library(${tgt} STATIC ${srcs})
    target_include_directories(${tgt} PUBLIC "${ARG_INCLUDE_DIR}")
    target_link_libraries(${tgt}
      PUBLIC  ${ARG_PUBLIC_LIBS}
      PRIVATE ${ARG_PRIVATE_LIBS} dualphoto_warnings)
  else()
    add_library(${tgt} INTERFACE)
    target_include_directories(${tgt} INTERFACE "${ARG_INCLUDE_DIR}")
    target_link_libraries(${tgt} INTERFACE ${ARG_PUBLIC_LIBS})
    message(STATUS "dualphoto: '${short_name}' has no sources yet (header-only placeholder)")
  endif()

  add_library(dualphoto::${short_name} ALIAS ${tgt})
endfunction()

# dualphoto_add_app(<name> [LIBS ...])
# Builds every .cpp in the current directory into executable dualphoto-<name>.
# Skipped (with a status message) while the directory has no sources.
# Also defines DUALPHOTO_HAVE_<BACKEND>=1 for every backend that was built,
# so an app can #ifdef on what is available.
function(dualphoto_add_app name)
  cmake_parse_arguments(ARG "" "" "LIBS" ${ARGN})

  file(GLOB_RECURSE srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
  if(NOT srcs)
    message(STATUS "dualphoto: app '${name}' has no sources yet, skipping")
    return()
  endif()

  set(tgt dualphoto_app_${name})
  add_executable(${tgt} ${srcs})
  set_target_properties(${tgt} PROPERTIES OUTPUT_NAME dualphoto-${name})
  target_link_libraries(${tgt} PRIVATE ${ARG_LIBS} dualphoto_warnings)

  foreach(b display_x11 display_sdl display_drm camera_opencv camera_v4l2 camera_gphoto2)
    if(TARGET dualphoto::${b})
      string(TOUPPER ${b} B)
      target_compile_definitions(${tgt} PRIVATE DUALPHOTO_HAVE_${B}=1)
    endif()
  endforeach()
endfunction()
