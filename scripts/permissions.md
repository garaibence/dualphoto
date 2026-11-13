# Device permissions (Linux)

| Device            | Needed for         | Fix                                                        |
|-------------------|--------------------|------------------------------------------------------------|
| `/dev/video*`     | webcam (V4L2/OpenCV) | `sudo usermod -aG video $USER`, then log out and back in |
| `/dev/dri/card*`  | DRM display backend | `sudo usermod -aG video $USER`; run from a TTY (Ctrl+Alt+F3) |
| `/dev/fb0`        | fbdev (if used)    | `video` group                                              |
| USB DSLR          | gphoto2 backend    | `plugdev`/udev rules; stop gvfs from auto-mounting the camera |

Check which webcam is which with `v4l2-ctl --list-devices` (package `v4l-utils`).
Lock exposure/white balance with e.g. `v4l2-ctl -d /dev/video0 --set-ctrl=auto_exposure=1`
(control names vary per camera; list them with `--list-ctrls`).
