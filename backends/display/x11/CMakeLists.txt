# Arch: pacman -S libx11 libxext libxrandr
find_package(X11 REQUIRED)
dualphoto_add_module(display_x11
  SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC_LIBS dualphoto::core
  PRIVATE_LIBS X11::X11 X11::Xext)
