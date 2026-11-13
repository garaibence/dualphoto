# dualphoto

A framework for capturing the light transport matrix **T** between a projector
and a camera, and using it for *dual photography* (images from the projector's
point of view) and relighting, based on
[Sen et al., "Dual Photography", SIGGRAPH 2005](https://dl.acm.org/doi/10.1145/1073204.1073257).

- **C++** does capture: pattern generation, display, camera, streaming T to disk.
- **Python** does processing: loading T, dual images, relighting, viewer.

> Status: project skeleton. The X11 fullscreen renderer is implemented; the
> remaining interfaces and backends are still in progress.

## Layout

| Path            | Purpose                                                        |
|-----------------|----------------------------------------------------------------|
| `core/`         | Hardware-free library: interfaces, scan runner, T writer       |
| `patterns/`     | `PatternSource` implementations (brute force, block, adaptive) |
| `backends/`     | Display (X11, SDL2, DRM) and camera (OpenCV, V4L2, gphoto2)    |
| `apps/`         | `scan`, `calibrate`, `preview` command-line tools              |
| `tests/`        | Fake camera/display and unit tests                             |
| `python/`       | Processing package (`pip install -e python`)                   |
| `docs/`         | Architecture, T file format, calibration                       |
| `data/`         | Scan sessions (gitignored)                                     |

Patterns and backends are independent axes: any pattern works with any
display and camera.

## Build

Dependencies on Arch Linux (install only what you enable):

```sh
sudo pacman -S base-devel cmake ninja        # toolchain
sudo pacman -S opencv                        # camera: OpenCV (default)
sudo pacman -S libx11 libxext libxrandr      # display: X11 (default)
sudo pacman -S sdl2                          # display: SDL2 (default)
sudo pacman -S libdrm                        # display: DRM (optional)
sudo pacman -S libgphoto2                    # camera: DSLR (optional)
```

```sh
scripts/build.sh                 # Release build in ./build, runs tests
scripts/build.sh Debug           # Debug build
# or by hand:
cmake -S . -B build -G Ninja -DDUALPHOTO_WITH_DRM=ON
cmake --build build
```

### CMake options

| Option                     | Default | Meaning                         |
|----------------------------|---------|---------------------------------|
| `DUALPHOTO_WITH_X11`       | ON      | X11 display backend             |
| `DUALPHOTO_WITH_SDL`       | ON      | SDL2 display backend (Wayland)  |
| `DUALPHOTO_WITH_DRM`       | OFF     | DRM/KMS display backend         |
| `DUALPHOTO_WITH_OPENCV`    | ON      | OpenCV camera backend           |
| `DUALPHOTO_WITH_V4L2`      | OFF     | Raw V4L2 camera backend         |
| `DUALPHOTO_WITH_GPHOTO2`   | OFF     | gphoto2 camera backend          |
| `DUALPHOTO_BUILD_APPS`     | ON      | Build `apps/`                   |
| `DUALPHOTO_BUILD_TESTS`    | ON      | Build `tests/` and fake devices |

The build is designed to configure while directories are still empty: modules
without `.cpp` files become header-only placeholders, and apps without sources
are skipped. Dropping a `.cpp` into the right directory is enough; sources are
globbed (re-run `cmake` is not needed thanks to `CONFIGURE_DEPENDS`).

Each target is `dualphoto_<name>` with alias `dualphoto::<name>`, e.g.
`dualphoto::core`, `dualphoto::display_x11`. Apps get
`DUALPHOTO_HAVE_DISPLAY_X11=1` (etc.) compile definitions for each backend built.

### Renderer preview

The `dualphoto-preview` app cycles through color bars, a grayscale ramp, a
checkerboard, and RGB gradients to smoke-test the implemented display renderer.
On X11, run it with:

```sh
./build/apps/preview/dualphoto-preview
./build/apps/preview/dualphoto-preview --output HDMI-1
```

Use `--display DISPLAY` to select an X server, and press Ctrl+C to stop the
preview. Other display backends can be added to this app as their renderers are
implemented.

### Python

```sh
python -m venv .venv && source .venv/bin/activate
pip install -e "python[viewer,dev]"
```

## Display backends and Wayland

- **X11**: RGB8 fullscreen rendering in an override-redirect, borderless window.
  `dualphoto::X11Renderer(nullptr, "HDMI-1")` selects an active output, and
  `present(width, height, rgb)` scales a packed RGB8 frame to that output.
  Best on X11 sessions.
- **SDL2**: fullscreen on a chosen output; works under Wayland and X11.
- **DRM**: no compositor at all; run from a TTY.

Check your session with `echo $XDG_SESSION_TYPE`.

## Setup tips

See [`docs/calibration.md`](docs/calibration.md) and
[`scripts/permissions.md`](scripts/permissions.md). In short: dark room, nothing
moves, lock the camera's exposure/white balance, run the projector at native
resolution with no scaling.

## Roadmap

1. `docs/t_format.md`: settle the on-disk format
2. Core interfaces + fake camera/display + brute-force pattern
3. X11/SDL display and OpenCV camera backends
4. Fixed block scan (binary block IDs + Hamming)
5. Grayscale / RGB scans
6. Adaptive hierarchical scan (quadtree + conflict graph coloring)
7. gphoto2 backend (Canon 600D)

## License

MIT, see [LICENSE](LICENSE).
