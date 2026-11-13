# Arch: pacman -S sdl2
find_package(SDL2 REQUIRED)
dualphoto_add_module(display_sdl
  SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC_LIBS dualphoto::core
  PRIVATE_LIBS SDL2::SDL2)
