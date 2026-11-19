> [!IMPORTANT]  
> This project was developed with AI coding assistance and has not yet undergone a full hardware soak test on physical CM0 hardware. It is provided under the Apache 2.0 license and may require adjustment for other environments.

# Building & Installation Guide

This guide covers building the CardputerZero ARM64 `.deb` package, installing on device with `APPLaunch` integration, building the macOS/Linux SDL2 desktop simulator, and running all verification suites.

---

## 1. Prerequisites

### CardputerZero Device (Runtime)
- Raspberry Pi CM0 (ARM64 Debian Bookworm, `dtoverlay=imx219` enabled in `/boot/firmware/config.txt`)
- Sony IMX219 camera module verified with `rpicam-hello --list-cameras`
- `APPLaunch` application launcher and `/dev/fb0` + `cp0-keys` `evdev` input device
- Runtime `.deb` dependencies (declared in [`packaging/debian/control`](../packaging/debian/control)):
  - `python3 (>= 3.9)`
  - `python3-picamera2`
  - `ffmpeg`
  - `alsa-utils` (`aplay`)
  - `libjpeg62-turbo | libjpeg8`
  - `libfreetype6`
  - `libc6`, `libstdc++6`

> Note: `czcamera` intentionally has **no** dependency on `cp0-sync` and **no** direct C++ linkage to `libcamera0*` or `libcamera.so`.

### Docker Cross-Build Host (macOS / Linux)
- Docker Desktop (or Docker Engine with `linux/arm64` emulation / `buildx`)

### Desktop Simulator Build (macOS / Linux)
- CMake `>= 3.16` and a C++17 compiler (`clang++` or `g++`)
- `pkg-config`
- `libjpeg` (`libjpeg-turbo` via Homebrew `jpeg-turbo`/`libjpeg` or Debian `libjpeg62-turbo-dev`)
- `SDL2` (`sdl2` via Homebrew or `libsdl2-dev`)
- **Optional:** `FreeType2` (`freetype` / `libfreetype-dev`) for crisp antialiased UI fonts (falls back automatically to the built-in `5×7` bitmap renderer if absent)
- **Optional:** `ffmpeg` & `ffprobe` on `PATH` for desktop Timelapse MP4 compilation and Gallery `.mp4` playback tests
- Python `>= 3.9` for running helper/packaging test suites

---

## 2. Building the ARM64 Debian Package (`.deb`)

Use [`packaging/build-deb.sh`](../packaging/build-deb.sh) in one of four supported modes:

```bash
./packaging/build-deb.sh --help
```

### Option A: Docker `linux/arm64` Build (Recommended on macOS / x86_64)

Builds a clean `debian:bookworm-slim` (`linux/arm64`) container via [`packaging/Dockerfile.arm64`](../packaging/Dockerfile.arm64), compiles `czcamera` with `-DCZ_BUILD_APP=ON -DCZ_TARGET_DEVICE=ON -DCZ_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE="-O2"`, executes ARM64 `ctest` and `cam-helper/tests`, and outputs `packaging/dist/czcamera_1.0.0_m5stack1_arm64.deb`:

```bash
./packaging/build-deb.sh
```

> Why `-O2` without global `-DNDEBUG`: the ARM64 verification step runs the C++ `ctest` binaries (`app/tests/*.cpp`) built in the same tree, and those tests use `<cassert>` `assert()`.

### Option B: Native Build on CardputerZero / ARM64 Linux

On an ARM64 Debian host with build dependencies installed (`build-essential cmake pkg-config libjpeg62-turbo-dev libfreetype-dev dpkg-dev`):

```bash
./packaging/build-deb.sh --local
```

### Option C: Stage Directory or Supply Prebuilt Binary

Stage the package tree without running `dpkg-deb` or Docker (useful for inspection or custom packaging):

```bash
./packaging/build-deb.sh --stage-only /tmp/czcamera-stage --bin /path/to/czcamera
```

Flags supported by `build-deb.sh`:

| Flag | Description |
|---|---|
| `--local` | Build `app/build-arm64` natively on Linux ARM64 and package `.deb` |
| `--stage-only <dir>` | Stage package files into `<dir>` with target permissions and exit |
| `--bin <path>` / `--ui-bin <path>` | Supply a prebuilt `czcamera` binary (`--ui-bin` is an alias for `--bin`) |
| `--out-dir <dir>` | Output directory for `.deb` (default: `packaging/dist/`) |
| `-h`, `--help` | Show usage help |

### Optional TrueType Font Bundling

If TTF/OTF font files exist under `app/assets/fonts/` or `packaging/fonts/` before running `build-deb.sh`, they are staged into `/usr/share/czcamera/fonts/` (`0644`). At runtime, `cz::AppView` checks `/usr/share/czcamera/fonts/` first, then common system font directories, and falls back to the built-in `5×7` bitmap font cleanly if none are installed.

---

## 3. Installing & Uninstalling on CardputerZero

Copy the generated package to the device and install via `dpkg` + `apt-get`:

```bash
scp packaging/dist/czcamera_1.0.0_m5stack1_arm64.deb pi@<cardputerzero-host>:/tmp/
ssh pi@<cardputerzero-host> 'sudo dpkg -i /tmp/czcamera_1.0.0_m5stack1_arm64.deb && sudo apt-get install -f'
```

### Installed Layout

| Destination Path | Mode | Role |
|---|---|---|
| `/usr/bin/czcamera` | `0755` | C++17 `320×170` `fbdev` + `evdev` camera application binary |
| `/usr/bin/czcamera-cam` | `0755` | Python 3 camera worker launcher (`python3-picamera2`) |
| `/usr/lib/czcamera/czcamera_cam/*.py` | `0644` | Protocol, `FramePipeWriter`, `Picam2Worker`, and mock driver modules |
| `/usr/share/APPLaunch/applications/czcamera.desktop` | `0644` | `APPLaunch` launcher entry (`Name=czcamera`, `Exec=/usr/bin/czcamera`) |
| `/usr/share/APPLaunch/share/images/czcamera.png` | `0644` | `80×80` Zinc dark camera launcher icon |
| `/usr/share/czcamera/fonts/*.ttf` *(optional)* | `0644` | Optional bundled UI fonts |

### Sanity Checks on Device

Verify shared libraries (`libjpeg.so.62` / `libfreetype.so.6` present, zero `libcamera.so` linkage) and camera visibility:

```bash
ldd /usr/bin/czcamera
rpicam-hello --list-cameras
```

Launch **czcamera** from the `APPLaunch` grid, or test from TTY/SSH:

```bash
/usr/bin/czcamera
```

### Uninstalling

```bash
sudo apt-get remove czcamera
```

[`packaging/debian/prerm`](../packaging/debian/prerm) automatically cleans Python `__pycache__` bytecode under `/usr/lib/czcamera` on package removal or deconfiguration.

---

## 4. Building & Running the Desktop SDL2 Simulator

Configure and build on macOS or Linux desktop:

```bash
cmake -S app -B app/build -DCZ_BUILD_APP=ON -DCZ_BUILD_TESTS=ON
cmake --build app/build
```

Run the interactive `320×170` SDL2 simulator (defaults to `SimCameraBackend` when `--sim` or `--mock-cam` is given):

```bash
./app/build/czcamera --sim --scale 3
```

Render headless `P6 320×170` PPM snapshots (`163215` bytes each) for CI or visual inspection:

```bash
./app/build/czcamera --render-test /tmp/cz_photo.ppm --theme dark --mode photo --rail right
./app/build/czcamera --render-test /tmp/cz_light_left.ppm --theme light --mode photo --rail left
./app/build/czcamera --render-test /tmp/cz_video.ppm --theme dark --mode video
./app/build/czcamera --render-test /tmp/cz_timelapse.ppm --theme dark --mode timelapse
./app/build/czcamera --render-test /tmp/cz_gallery.ppm --theme dark --mode gallery
./app/build/czcamera --render-test /tmp/cz_settings.ppm --theme dark --mode settings
```

### CMake Build Options

| Option | Default | Effect |
|---|---|---|
| `CZ_BUILD_APP` | `OFF` | Build the `czcamera` executable in addition to `czcamera_core` |
| `CZ_TARGET_DEVICE` | `OFF` | Build Linux `/dev/fb0` + `cp0-keys` `evdev` backend instead of SDL2 (Linux only) |
| `CZ_BUILD_TESTS` | `ON` | Build the 5 `ctest` suites |
| `CZ_ENABLE_LVGL` | `OFF` | Link optional `lvgl::lvgl` (`CP0_HAS_LVGL=1`) if available |

---

## 5. Running Test Suites

Run all three suites cleanly from the repository root:

```bash
# C++ unit & integration tests (5 suites)
ctest --test-dir app/build --output-on-failure -V

# Python camera helper & mock-driver IPC tests (18 tests, warnings-as-errors)
PYTHONPATH=cam-helper python3 -W error -m unittest discover -s cam-helper/tests -v

# Packaging metadata, icon, maintainer script & staging tests (4 tests)
python3 -m unittest discover -s packaging/tests -v
```

*(Note: `test_backend_protocol` also spawns `cam-helper/bin/czcamera-cam --mock-driver` end-to-end across pipes to validate C++ ↔ Python JSON v1 + `CZFR` binary framing.)*
