# Every app links the core, the patterns, and whichever backends were enabled.
set(DUALPHOTO_APP_LIBS dualphoto::core dualphoto::patterns)
foreach(b display_x11 display_sdl display_drm camera_opencv camera_v4l2 camera_gphoto2)
  if(TARGET dualphoto::${b})
    list(APPEND DUALPHOTO_APP_LIBS dualphoto::${b})
  endif()
endforeach()

add_subdirectory(scan)
add_subdirectory(calibrate)
add_subdirectory(preview)
