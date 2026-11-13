# Raw V4L2 via <linux/videodev2.h>; no extra packages needed.
dualphoto_add_module(camera_v4l2
  SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
  PUBLIC_LIBS dualphoto::core)
