> [!IMPORTANT]  
> This project was developed entirely with AI coding assistance (Google Neon) and has not undergone rigorous review. It is provided as-is and may require adjustments for other environments.

# `czcamera`

`czcamera` is a camera application for the M5Stack CardputerZero that replaces the stock Camera app. It adds true cropped zoom, a separate focus loupe, nine live photo filters, video recording with microphone audio, foreground timelapse capture, and an on-device gallery with video playback.

## What it adds over the stock camera

| Feature | Stock Camera | `czcamera` |
|---|---|---|
| Photos | Unfiltered stills only | `8MP` / `2MP` stills, EV (`-2.0` to `+2.0`), `3s`/`10s` self-timer, grid overlay |
| Zoom and framing | Preview magnification only (saved photos stay uncropped) | Cropped zoom (`1.0x` to `4.0x`) in Photo, Video, and Timelapse, plus a `2x` viewfinder-only Loupe (`Z`) |
| Filters | None | 9 live-previewed filters baked into JPEGs (`None`, `B&W`, `Hi-Con`, `Sepia`, `Cool`, `Vivid`, `Invert`, `Retro`, `GameBoy`) |
| Video | None | `480p`, `720p`, or `1080p` `.mp4` at 30fps with mic audio (`M` mute toggle, muted fallback), live viewfinder, and live zoom/loupe |
| Timelapse | None | Custom intervals (`1-3600s`), `1-30` FPS, manual/shot/duration stop limits, raw JPEG folder or compiled `.mp4`, auto-dim with safe wake, crash recovery |
| Gallery and playback | Basic still viewing | Full-screen browser for photos, timelapse folders, and on-device `.mp4` playback with speaker audio, `-5s`/`+5s` seek, and `[Y/N]` delete confirmation |
| Interface and storage | Fixed layout and defaults | Dark/Light themes, left/right control rail swap, camera orientation options, configurable output directory (`~/Pictures/czcamera` default), and hidden staging files for external sync tools |

## Features and usage

The interface splits the screen between a live viewfinder and a control rail with tabs for Photo (`1`), Video (`2`), and Timelapse (`3`). Navigate rail cards and menus with the arrow keys or `h`/`j`/`k`/`l`, swap the rail between the left and right edges with `W`, switch between Dark and Light themes with `T`, and toggle the rule-of-thirds grid with `G`.

### Photo (`1`)

Photo mode saves `8MP` (default) or `2MP` (`R`) JPEGs with EV adjustment (`-2.0` to `+2.0`), an `Off`/`3s`/`10s` self-timer (`C`), `1.0x` to `4.0x` cropped zoom (`5`/`7`), and a `2x` viewfinder-only Loupe (`Z`) to check focus without cropping the final image. Pressing `F` cycles all nine filters on the live preview; `None` saves the captured JPEG directly without C++ re-encoding.

### Video (`2`)

In Video (`2`), pressing `Enter`, `Space`, or `6` records `720p30` (default), `1080p30`, or `480p30` `.mp4` clips (`R` cycles resolution while idle). The microphone is on by default (`M` toggles mute while idle), and if audio capture is unavailable the app falls back to recording a silent `.mp4`. Zoom (`5`/`7`) and Loupe (`Z`) stay active during recording, which stops automatically if free space drops below `100 MB`.

### Timelapse (`3`)

Timelapse shoots `2MP` (default), `1080p`, or `8MP` (`R`) sequences using preset or custom intervals (`1-3600s` via `C` or `P`), `1-30` playback FPS, and manual, shot-count, or duration stop limits. Sessions save as a `TL_<timestamp>/` JPEG folder or compile into `TL_<timestamp>.mp4`. During a run, the screen dims automatically, any keypress wakes the display first without cancelling capture, and unfinished sessions trigger a recovery prompt on the next launch.

### Gallery (`8`) and settings (`S`)

Press `8` to pause the camera and open the full-screen gallery, sorted newest first. From there you can open `TL_*` folders, play `.mp4` files with speaker audio, seek `-5s` or `+5s`, and delete items after a `[Y/N]` confirmation. Settings (`S`) covers the output directory (folder tree picker or manual path entry via `T`), rail side, theme, grid overlay, timelapse auto-dim timeout, and camera orientation (`auto`, `0`, `180`, `hflip`, `vflip`).

## Installation

Build the ARM64 `.deb` package on macOS or Linux with Docker (`linux/arm64` enabled), or pass `--local` to build directly on the CardputerZero:

```bash
./packaging/build-deb.sh
scp packaging/dist/czcamera_1.0.0_m5stack1_arm64.deb pi@<cardputerzero-host>:/tmp/
```

Install the package on the device:

```bash
sudo dpkg -i /tmp/czcamera_1.0.0_m5stack1_arm64.deb
sudo apt-get install -f
```

Launch `czcamera` from `APPLaunch` or run `/usr/bin/czcamera`. Remove it with `sudo apt-get remove czcamera`.

For the desktop SDL2 simulator, native device builds, and unit tests, see [Building, installation, and desktop simulator](docs/building-and-installation.md).

## Keyboard shortcuts

| Key | Context | Action |
|---|---|---|
| `1`, `2`, `3` | Main view / Gallery | Switch to Photo, Video, or Timelapse |
| `Enter`, `Space`, `6` | Main view | Take photo, start/stop video, or start timelapse / open stop prompt |
| `Up`/`Down` or `k`/`j` | Rail / Lists / Modals | Move cursor up or down |
| `Left`/`Right` or `h`/`l` | Rail / Settings / Video playback | Cycle selected setting, or seek `-5s`/`+5s` during video playback |
| `5` / `7` | Photo / Video / Timelapse | Step cropped digital zoom (`1.0x` to `4.0x`, works during video and timelapse) |
| `Z` | Photo / Video / Timelapse | Toggle `2x` viewfinder-only Loupe (idle or active capture) |
| `F` | Photo | Cycle 9 photo filters (`None` through `GameBoy`) |
| `R` | Main view / Offline | Cycle resolution preset, or retry camera startup when `Offline` |
| `C` (or `P` in TL) | Photo / Timelapse | Cycle self-timer (`Off`/`3s`/`10s`) in Photo; open custom setup in Timelapse |
| `M` | Video (idle) / Gallery video | Toggle microphone recording / Toggle playback mute |
| `G` | Main view | Toggle rule-of-thirds grid |
| `W` | Main view | Flip control rail between right and left |
| `T` | Main view / Gallery / DirPicker | Toggle Dark / Light theme (or enter path manually in DirPicker) |
| `8` | Main view | Pause camera and open Gallery |
| `S` | Main view | Open Settings |
| `D` / `Delete` | Gallery | Prompt `[Y/N]` to delete selected item |
| `Esc` / `Q` | Dimmed TL / Modals / Main | Wake dimmed screen safely (first press), close/cancel modal, or exit |

## Troubleshooting

| Symptom | Check or fix |
|---|---|
| `Camera Offline [R]` on launch | `czcamera` shows `"Connecting..."` and auto-retries up to 3 times before displaying the error reason below `Camera Offline [R]`. Press `R` to retry, and check `~/.local/share/czcamera/czcamera.log`, `grep imx219 /boot/firmware/config.txt`, and `rpicam-hello --list-cameras`. |
| Video records without audio (`audio_unavailable`) | When audio capture fails, `czcamera` records a silent `.mp4` instead of dropping the clip. Check ALSA inputs with `arecord -l` and `alsamixer`. |
| Upside-down or mirrored preview | Open Settings (`S`) and cycle `Camera Orient` (`auto`, `0`, `180`, `hflip`, `vflip`). |
| Recovery prompt appears on startup | An earlier timelapse ended before finishing. Press `C` to compile frames to `.mp4`, `K` to keep the raw JPEGs, `D` to discard them, or `Esc` to defer. |
| Capture refuses to start or stops early | `czcamera` checks for at least `100 MB` free on the target mount before starting and during video or timelapse runs. |

## Documentation

- [Usage, controls, and configuration](docs/usage-and-controls.md): Mode behavior, all nine filters, `config.json` fields, CLI flags, and sync-safe file naming.
- [Building, installation, and desktop simulator](docs/building-and-installation.md): Dependencies, `.deb` packaging options, SDL2 simulator, headless PPM snapshots, and test suites.
- [Architecture and camera protocol](docs/architecture-and-protocol.md): Backend interface, camera helper JSON and binary frame protocol, UI color tokens, and directory map.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE.md](LICENSE.md) for the full text.
