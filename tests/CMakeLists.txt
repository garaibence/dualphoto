# Fake devices let full scans run without hardware.
dualphoto_add_module(fake_camera
  SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fake_camera
  INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fake_camera
  PUBLIC_LIBS dualphoto::core)

dualphoto_add_module(fake_display
  SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fake_display
  INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fake_display
  PUBLIC_LIBS dualphoto::core)

# Any tests/<name>_test.cpp becomes its own executable and a CTest entry.
# Plain executables: exit non-zero on failure. Swap in Catch2/GoogleTest if you like.
file(GLOB test_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
foreach(src ${test_srcs})
  get_filename_component(test_name ${src} NAME_WE)
  add_executable(${test_name} ${src})
  target_link_libraries(${test_name} PRIVATE
    dualphoto::core dualphoto::patterns
    dualphoto::fake_camera dualphoto::fake_display
    dualphoto_warnings)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
