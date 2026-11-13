# Calibration and setup

Outline.

- Dark room, nothing moves between captures
- Projector at native resolution, 1:1 pixel mapping, no scaling or color management
- Camera: lock exposure, gain, white balance, focus; verify the lock holds
- Settle time: discard N frames after each pattern change
- Black level: capture while projecting black, subtract from every capture
- Projector linearization: gray ramp capture
- Webcam caveats: 8-bit, gamma-encoded, possibly MJPEG-compressed
