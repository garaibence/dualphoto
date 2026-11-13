# Calibration and setup

- Dark room, nothing moves between captures
- Projector at native resolution, 1:1 pixel mapping, no scaling or color management
- Camera: calibration first enables supported automatic controls under a white
  projection, waits for brightness to stabilize, then locks the resulting
  exposure, gain, white balance, and focus; verify the reported status
- Settle time: measure it with `dualphoto-calibrate`; discard stale frames after each pattern change
- Black level: capture while projecting black, subtract from every capture
- Projector linearization: gray ramp capture
- Webcam caveats: 8-bit, gamma-encoded, possibly MJPEG-compressed

## Software timing calibration

Build with both X11 and V4L2 enabled, then run `dualphoto-calibrate`. The app
projects alternating full-screen black and white frames and monitors the
center region of the camera image. It measures the host-observed time from
submitting a pattern to receiving three consecutive camera frames that pass a
90% brightness threshold. It reports p50/p95/maximum measurements and suggests
a wait of the observed maximum plus one measured camera-frame interval.

Capture defaults to 640x360, which can provide a higher camera frame rate than
720p on many webcams. The actual negotiated camera frame size is printed; use
`--width` and `--height` to select another supported mode. Before measuring,
the app projects white, enables supported auto-exposure, auto-gain,
auto-white-balance, and autofocus, and waits for six consecutive camera samples
to have stable center-region brightness (after at least two seconds). It then
disables automatic modes and searches manual exposure values for the shortest
setting that yields stable black/white captures and at least 20 luma contrast.
Each candidate discards six frames after each setting/pattern change, then
checks up to three six-frame windows. A stable sample window must vary by no
more than 3% (or 2 luma levels). A binary search assumes image signal generally
increases with exposure. The selected minimum is verified repeatedly; if it
does not remain stable, nearby longer exposures are checked before calibration
fails. The measured frame interval is reported.
For exposure, it queries the camera's advertised menu values and tries
automatic, aperture-priority, and shutter-priority modes rather than assuming
every device supports the same enum. A missing control or a failure to
stabilize is reported. Verify the resulting image and reported status before
relying on timing results.

This measures the combined projector, camera, driver, and host delivery path;
it cannot distinguish their individual delays. The recommendation is a
software estimate, not hardware synchronization or a hard timing guarantee.
Run several calibrations if timing varies between captures; base production
settling behavior on the worst observed result with an additional margin
appropriate to the hardware.
