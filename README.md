> [!IMPORTANT]  
> This project was developed with AI coding assistance and has not yet undergone a full hardware soak test on physical CM0 hardware. It is provided under the Apache 2.0 license and may require adjustment for other environments.

# `czcamera` — CardputerZero Camera App

A dedicated `320×170` camera, video, timelapse, and gallery application built for the **M5Stack CardputerZero** (Raspberry Pi CM0, ARM64 Debian Bookworm, non-touch `/dev/fb0`, 46-key `cp0-keys`, 8MP Sony IMX219 CSI-2 sensor, ES8389 MEMS microphone & speaker), styled with `shadcn/ui` Zinc Dark and Light themes.

Includes a desktop **SDL2 simulator** (`--sim`) with a procedural test scene (`--mock-cam`) and headless PPM snapshot runner (`--render-test`) for macOS and Linux development.

---

## Key Features

- **Option A Split Layout (`320×170`):** Live `226×170` viewfinder alongside a focusable `94×170` control rail (`1P · 2V · 3T`), with live **`[W]`** rail-side toggle (`right` default or `left`) and **`[T]`** Zinc Dark / Light theme toggle.
- **Photo Mode + 9 Filters:** True ISP `ScalerCrop` zoom (`1x`–`4x` via `5`/`7`), independent `2x` viewfinder-only **Loupe** (`Z`), EV (`-2.0` to `+2.0`), `8MP` / `2MP` presets (`R`), `Off`/`3s`/`10s` self-timer (`C`), `[G]` rule-of-thirds grid, and 9 shared RGB24/`libjpeg` filters (`None` zero-re-encode passthrough, `B&W`, `Hi-Con`, `Sepia`, `Cool`, `Vivid`, `Invert`, `Retro`, and Bayer-dithered `GameBoy` DMG).
- **Hardware H.264 Video Mode:** `720p30` (default), `1080p30`, and `480p30` `.mp4` recording with ES8389 MEMS mic ON by default (`[M]` idle mute toggle, graceful muted fallback on `audio_unavailable`), live 16:9 letterboxed viewfinder, and live digital zoom (`5`/`7`) and `2x` Loupe (`Z`) during capture.
- **Foreground Timelapse Mode:** `2MP` default (`1080p` / `8MP` presets), customizable interval, any FPS `1..30`, and stop presets (`C` / `P`), live zoom (`5`/`7`) & `2x` Loupe (`Z`), output to `TL_<timestamp>/` JPEGs or compiled `TL_<timestamp>.mp4` (`h264_v4l2m2m` at memory-safe `1024×768` / `1280×720` with low-RAM `libx264` fallback), sysfs backlight auto-dim + **dimmed safe-wake** (any keypress only wakes the display), and startup recovery for orphaned `.tl_work_*` sessions.
- **Full-Screen Gallery & Video Playback (`[8]`):** Pauses the camera backend while open, supports `TL_*` session drill-down, plays `.mp4` videos (with or without audio tracks) via `ffmpeg`/`aplay` (or SDL2 audio on desktop) with `-5s`/`+5s` seek, and enforces `[Y/N]` delete confirmation.
- **Settings & Directory Tree Picker (`[S]`):** Output folder browser (plus `[T]` manual path entry), `Camera Orient` (`auto`, `0°`, `180°`, `H-Flip`, `V-Flip` via ISP `Transform`), rail side, theme, grid, and auto-dim timeout.
- **Sync-Safe & Hardware-Decoupled:** Writes exclusively to `.`-prefixed staging paths until finalized via `fsync` + atomic `rename()` (with a `100 MB` free-space guard), keeping zero runtime/packaging dependencies on external sync tools or direct `libcamera.so` linkage.

---

## Architecture Overview

| Component | Path | Target / Role | Purpose |
|---|---|---|---|
| **C++17 UI & Core** | [`app/`](app/) | CardputerZero (`/dev/fb0` + `cp0-keys`) & macOS (`--sim`) | `czcamera` binary & `czcamera_core` library (`ShadcnCanvas`, `UiController`, filters/JPEG, timelapse engine, gallery model, `VideoPlayer`, and 5 CTest suites). |
| **Python Camera Worker** | [`cam-helper/`](cam-helper/) | CardputerZero (`czcamera-cam`) & `--mock-driver` | Dual-stream (`main` + `lores`) `python3-picamera2` worker driven by newline JSON v1 + 16-byte `CZFR` binary frame pipe. |
| **ARM64 Packaging** | [`packaging/`](packaging/) | ARM64 Docker (`bookworm-slim`) / Debian | Builds `czcamera_1.0.0_m5stack1_arm64.deb` installing `czcamera`, `czcamera-cam`, `czcamera_cam`, and the `APPLaunch` entry/icon. |

---

## Quick Start

### 1. Build & Install on CardputerZero (Docker on macOS/Linux → CM0)

Ensure Docker Desktop is running with `linux/arm64` support, then build `packaging/dist/czcamera_1.0.0_m5stack1_arm64.deb`:

```bash
./packaging/build-deb.sh
scp packaging/dist/czcamera_1.0.0_m5stack1_arm64.deb pi@<cardputerzero-host>:/tmp/
```

Install on the CardputerZero:

```bash
sudo dpkg -i /tmp/czcamera_1.0.0_m5stack1_arm64.deb
sudo apt-get install -f
```

Launch **czcamera** from the `APPLaunch` app grid or run `/usr/bin/czcamera` directly.

### 2. Run the Desktop Simulator (macOS / Linux)

```bash
cmake -S app -B app/build -DCZ_BUILD_APP=ON -DCZ_BUILD_TESTS=ON
cmake --build app/build
./app/build/czcamera --sim --scale 3
```

Run the unit test suites anytime:

```bash
ctest --test-dir app/build --output-on-failure
PYTHONPATH=cam-helper python3 -W error -m unittest discover -s cam-helper/tests -v
python3 -m unittest discover -s packaging/tests -v
```

---

## Core Keyboard Shortcuts (`320×170`)

| Key | Where | Action |
|---|---|---|
| `1`, `2`, `3` | Main View | Switch between **Photo**, **Video**, and **Timelapse** |
| `Enter`, `Space`, `6` | Main View | Trigger shutter / Start-Stop recording / Open Timelapse stop prompt |
| `Up`/`Down` or `k`/`j` | Rail / Lists / Modals | Move selection cursor |
| `Left`/`Right` or `h`/`l` | Rail / Settings | Cycle selected card value |
| `5` / `7` | Photo / Video / Timelapse | Step ISP digital crop zoom (`1.0x`–`4.0x`, live during Video & Timelapse) |
| `Z` | Photo / Video / Timelapse | Toggle `2x` viewfinder-only Loupe (works idle or recording/capturing) |
| `F` | Photo | Cycle 9 photo filters (`None` .. `GameBoy`) |
| `R` | Photo / Video / TL (or Offline) | Cycle resolution preset (or recover camera when `Offline`) |
| `C` | Photo / Timelapse | Cycle self-timer (`Off`/`3s`/`10s`) in Photo; open Custom TL modal in Timelapse |
| `M` | Video (idle) / Gallery video | Toggle ES8389 mic audio / Toggle playback mute |
| `G` | Main View | Toggle rule-of-thirds viewfinder grid |
| `W` | Main View | Swap control rail side (`right` ↔ `left`) |
| `T` | Main View / Gallery / DirPicker | Toggle Zinc Dark / Light theme (or type path inside DirPicker) |
| `8` | Main View | Pause camera and open full-screen **Gallery** |
| `S` | Main View | Open **Settings** modal |
| `Esc` / `Q` | Dimmed Timelapse / Modals / Main | Wake dimmed screen safely (first key), cancel/close modal, or exit to `APPLaunch` |

---

## Documentation

- **[Building & Installation Guide](docs/building-and-installation.md)** — Host & device dependencies, `.deb` packaging (`./packaging/build-deb.sh` modes), `APPLaunch` integration, font bundling, SDL2 simulator builds, and C++/Python/packaging tests.
- **[Usage, Controls & Configuration Guide](docs/usage-and-controls.md)** — Capture modes, all 9 photo filters, Timelapse auto-dim safe-wake & recovery, Gallery playback, complete keymap, `config.json` (18 fields), CLI flags, sync-safe staging rules, and troubleshooting.
- **[Architecture & Camera Protocol Reference](docs/architecture-and-protocol.md)** — `cz::ICameraBackend` decoupling boundary, JSON v1 + 16-byte `CZFR` binary frame pipe spec, dual `main`+`lores` camera pipeline, VideoCore IV limits, `320×170` Zinc tokens, and directory map.
- **[Design Specification](docs/superpowers/specs/2026-11-19-czcamera-design.md)** — Original hardware/UI design specification.
- **[Implementation Plan](docs/superpowers/plans/2026-11-19-czcamera.md)** — 7-task TDD architecture and verification plan.

---

## Repository Layout

```text
.
├── app/                # C++17 320x170 UI, filters, timelapse/gallery/playback & 5 CTest suites
├── cam-helper/         # Python 3 czcamera-cam helper (picamera2 + mock driver) & tests
├── packaging/          # ARM64 Docker/.deb builder, APPLaunch desktop/icon & packaging tests
├── docs/               # User/developer guides, design specification & implementation plan
├── chat-summaries/     # Technical implementation summaries
└── LICENSE.md          # Apache License 2.0
```

---

## License

Licensed under the **Apache License, Version 2.0**. See [LICENSE.md](LICENSE.md) for the full license text.
