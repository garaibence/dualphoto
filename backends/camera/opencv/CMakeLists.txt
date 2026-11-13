# Arch: pacman -S opencv
find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)
dualphoto_add_module(camera_opencv
  SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC_LIBS dualphoto::core
  PRIVATE_LIBS ${OpenCV_LIBS})
