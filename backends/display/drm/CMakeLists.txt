# Arch: pacman -S libdrm   (run from a TTY, no X/Wayland session on that VT)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
dualphoto_add_module(display_drm
  SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC_LIBS dualphoto::core
  PRIVATE_LIBS PkgConfig::LIBDRM)
