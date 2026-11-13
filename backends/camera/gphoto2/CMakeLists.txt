# Arch: pacman -S libgphoto2
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPHOTO2 REQUIRED IMPORTED_TARGET libgphoto2)
dualphoto_add_module(camera_gphoto2
  SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC_LIBS dualphoto::core
  PRIVATE_LIBS PkgConfig::GPHOTO2)
