> [!IMPORTANT]  
> This project was developed entirely with AI coding assistance (Google Neon) and has not undergone rigorous review. It is provided as-is and may require adjustments for other environments.

# Usage, Controls & Configuration Guide

`czcamera` is operated entirely with the 46-key CardputerZero keyboard (`cp0-keys`) or an attached keyboard in the SDL2 desktop simulator.

---

## 1. Screen Layout (Option A Split View)

The `320×170` canvas splits into two regions whose horizontal side swaps dynamically with **`W`** (or via Settings → `Control Rail`):

- **Viewfinder (`226×170`):** `16:9` modes (`720p`, `1080p` Video, `1080p` Timelapse) letterbox cleanly into `226×127` centered on a `#050507` surround; `4:3` modes (`8MP`, `2MP`, `480p`) fill `226×170`. Minimal corner badges show active filter, crop zoom / `2x` Loupe state, timer countdown, `● REC MM:SS`, or Timelapse shot counter/next-shot countdown.
- **Control Rail (`94×170`):** Displays `1P · 2V · 3T` mode tabs at the top, 4 setting cards tailored to the active mode, and a compact 2-line `<kbd>` keybind hint footer.

---

## 2. Capture Modes

### Photo Mode (`1`)
- **Resolutions (`R` or card 0):** `8MP` (`3280×2464` JPEG) default, `2MP` (`1640×1232` JPEG).
- **Digital Crop Zoom vs. Viewfinder Loupe:**
  - **`5` / `7`**: Steps ISP `ScalerCrop` (`1.0x`, `1.5x`, `2.0x`, `3.0x`, `4.0x`). Both the live preview and captured media are cropped cleanly to that region (active across Photo, Video, and Timelapse).
  - **`Z`**: Toggles a **`2x` Viewfinder-Only Loupe** for checking framing/focus in Photo, Video, or Timelapse (idle or active capture) without cropping saved images/frames. Lowercase `h` / `l` remain dedicated to `Left` / `Right` `hjkl` navigation everywhere.
- **Exposure Value (card 1, `Left`/`Right`):** `-2.0` to `+2.0` in `0.5` EV steps.
- **Photo Filters (`F` or card 2):** Cycles all 9 filters live on the preview and applies them on JPEG save (`None` writes the sensor JPEG directly with zero re-encode):
  `None` (`none`), `B&W` (`bw`), `Hi-Con` (`hicon`), `Sepia` (`sepia`), `Cool` (`cool`), `Vivid` (`vivid`), `Invert` (`invert`), `Retro` (`retro`), `GameBoy` (`gameboy`, scaled integer blocks with 4-shade Bayer DMG palette).
- **Self-Timer (`C` or card 3):** Cycles `Off` (`0s`), `3s`, `10s`. Press `Esc` during countdown to cancel without quitting.
- **Shutter (`Enter`, `Space`, `6`):** Stages to `.IMG_<ts>.jpg.tmp`, applies filter if active, `fsync`s, and commits atomically to `IMG_<ts>.jpg` (adding `_01`, `_02`, ... on same-second collisions).

### Video Mode (`2`)
- **Resolutions (`R` or card 0, while idle):** `720p` (`1280×720` @ 30fps, default), `1080p` (`1920×1080` @ 30fps), `480p` (`640×480` @ 30fps).
- **Audio (`M` or card 1, while idle):** ES8389 MEMS microphone ON by default (`audio=true`). If ALSA capture is unavailable or busy at recording start, `czcamera-cam` emits `audio_unavailable`, shows a warning toast, and falls back to muted `.mp4` recording instead of aborting.
- **Live Zoom & Loupe (`5` / `7` and `Z`):** Zoom (`1.0x`–`4.0x`) and `2x` Loupe remain active both before and during recording.
- **Storage Indicator (card 3):** Displays read-only `FREE` space remaining on the target mount (`100 MB` minimum enforced every 2 seconds while recording; auto-stops safely if space drops below `100 MB`).
- **Recording Lock:** While `● REC` is active, mode switches (`1`/`2`/`3`), `[8]` Gallery, `[S]` Settings, `[W]` rail flip, `[T]` theme toggle, `[M]`/`[R]`, and `Q` are locked out. Press `Enter`, `Space`, `6`, or `Esc` to stop recording cleanly.

### Timelapse Mode (`3`)
- **Defaults & Presets:**
  - **Resolution (`R` or card 2):** `2MP` (`1640×1232` default for tighter intervals), `1080p` (`1920×1080`), `8MP` (`3280×2464`).
  - **Interval (card 0):** Cycles `2s`, `5s` (default), `10s`, `30s`, `60s`, `300s` (or any custom `1..3600s` via `[C]` / `[P]`).
  - **Output Mode (card 1):** `video` (`TL_<ts>.mp4`, default) or `photos` (`TL_<ts>/frame_0001.jpg`...).
  - **Stop Condition (card 3):** `Manual` (default), `60 shots`, `120 shots`, `300 shots`, `15 min`, `30 min`, `60 min` (or custom via `[C]` / `[P]`).
- **Live Zoom & Loupe (`5` / `7` and `Z`):** Adjust crop zoom (`1.0x`–`4.0x`) or toggle `2x` viewfinder Loupe both before and during active timelapse capture.
- **Custom Input Modal (`C` or `P` while idle):** Adjusts exact Interval (`1..3600s`), Video FPS (`1..30` via `Left`/`Right`/`h`/`l` `±1` stepping or typing digits `0-9` + `Backspace`), Stop Mode (`manual` / `shots` / `duration`), and Stop Value (`1..9999` shots or `1..1440` min). `Enter` saves and closes; `Esc` discards.
- **MP4 Compilation & CM0 Memory/V4L2 Compliance:**
  - Frames stage inside `.tl_work_<ts>/` with a `manifest.json` heartbeat.
  - On stop/completion in `video` mode, `ffmpeg` runs single-threaded input/filter decode (`-threads 1 -filter_threads 1`), scales frames cleanly to CM0 hardware-friendly bounds (`1024×768` bilinear for `4:3` `2mp`/`8mp`, `1280×720` bilinear for `16:9` `1080p`), encodes via `h264_v4l2m2m` (`-num_output_buffers 4`, `5M`, `yuv420p`), and automatically falls back to low-RAM `libx264` (`-threads 2 -preset ultrafast -crf 23 -x264-params rc-lookahead=0:ref=1:bframes=0:...`) if hardware init fails.
  - Individual `Photos`-mode frames remain full native capture resolution (`2MP`, `1080p`, or `8MP`).
  - Press `Esc` during `TimelapseCompiling` to cancel compilation cleanly and preserve the raw frames in a non-hidden `TL_<ts>/` folder.
- **Auto-Dim & Safe Wake:**
  - When capturing, the screen dims after `timelapse_dim_sec` idle seconds (`15s` default; configurable `15s`/`30s`/`60s`/`Off` in Settings) via `/sys/class/backlight/*/brightness` (or a software black frame fallback on desktop/headless).
  - **Safe-Wake Rule:** Pressing **any** key while dimmed (including `Enter`, `6`, `Esc`, `Q`) **only wakes the display** and resets the idle timer — and on Linux `evdev`, held auto-repeats (`ev.value == 2`) for that wake key are suppressed until key release (`ev.value == 0`). Press `Esc`, `Enter`, `6`, or `Q` while **awake** to open `TimelapseStopConfirm` (`[Y]` stop & finish/compile, `[N]`/`[Esc]` keep capturing).
- **Crash Recovery on Launch:** On startup, `czcamera` scans `output_dir` for incomplete `.tl_work_*` directories containing valid JPEGs and prompts `TimelapseRecovery`: `[C]` Compile to MP4, `[K]` Keep Photos, `[D]` Discard, or `[Esc]` Defer.

---

## 3. Gallery & Media Playback (`8`)

Pressing `8` pauses the camera backend (`pause` IPC, releasing the sensor) and opens the full-screen `320×170` Gallery showing items from `output_dir` (newest first, ignoring all `.`-prefixed hidden files/directories):

| Key | Gallery Context | Action |
|---|---|---|
| `Up`/`Down`/`Left`/`Right` or `k`/`j`/`h`/`l` | Photo / Video (stopped) / TL Folder | Navigate items |
| `Space` / `Enter` | `TL_*` Timelapse Folder | Drill into folder (`Backspace` or `Esc` returns to root) |
| `Space` / `Enter` | Video (`.mp4`) | Play / Pause |
| `Left` / `Right` or `h` / `l` | Video (playing) | Seek `-5s` / `+5s` |
| `M` | Video selected | Toggle playback mute (works cleanly even on silent `.mp4` files with no audio stream) |
| `D` / `Delete` | Any item | Open `[Y]` Confirm / `[N]`/`Esc` Cancel delete prompt |
| `T` | Gallery | Toggle Zinc Dark / Light theme |
| `1`, `2`, `3` | Gallery | Close Gallery, resume camera, and jump directly to mode |
| `Esc` / `Q` | Gallery root | Close Gallery and resume camera live preview |

---

## 4. Settings Modal & Directory Tree Picker (`S`)

| Row | Setting | Values / Action |
|---|---|---|
| 0 | `Output Dir` | Opens `DirPicker` (`Enter` drill down, `Backspace` parent, `Space` select highlighted dir, `.` select current dir, `/` root, `~` home, `[T]` manual `TextInput` with printable ASCII + spaces, `Esc` back) |
| 1 | `Control Rail` | `right` (default) / `left` |
| 2 | `Theme` | `dark` (Zinc Dark) / `light` (Zinc Light) |
| 3 | `Grid Overlay` | `on` (`true`) / `off` (`false`) |
| 4 | `Timelapse Dim` | `15s` / `30s` / `60s` / `Off` (`0`) |
| 5 | `Camera Orient` | `auto` / `0` / `180` / `hflip` / `vflip` (live backend `reconfigure`) |

---

## 5. Configuration Reference (`config.json`)

Persisted atomically to `~/.config/czcamera/config.json` (override path with `CZCAMERA_CONFIG`). Malformed or missing configs fall back safely to defaults; out-of-range fields are sanitized on load.

| Field | Default | Valid Values / Range |
|---|---|---|
| `output_dir` | `"~/Pictures/czcamera"` | Target media folder (`~/` expanded via `$HOME`) |
| `theme` | `"dark"` | `"dark"`, `"light"` |
| `rail_side` | `"right"` | `"right"`, `"left"` |
| `photo_resolution` | `"8mp"` | `"8mp"` (`3280×2464`), `"2mp"` (`1640×1232`) |
| `photo_filter` | `"none"` | `"none"`, `"bw"`, `"hicon"`, `"sepia"`, `"cool"`, `"vivid"`, `"invert"`, `"retro"`, `"gameboy"` |
| `photo_timer_sec` | `0` | `0`, `3`, `10` |
| `grid_enabled` | `true` | `true`, `false` |
| `video_resolution` | `"720p"` | `"480p"`, `"720p"`, `"1080p"` |
| `video_audio` | `true` | `true`, `false` |
| `timelapse_resolution` | `"2mp"` | `"2mp"`, `"1080p"`, `"8mp"` |
| `timelapse_interval_sec` | `5` | `1..3600` (seconds) |
| `timelapse_output_mode` | `"video"` | `"video"`, `"photos"` |
| `timelapse_fps` | `24` | `1..30` |
| `timelapse_stop_mode` | `"manual"` | `"manual"`, `"shots"`, `"duration"` |
| `timelapse_stop_value` | `0` | `>= 0` (shot count or seconds when `"duration"`) |
| `timelapse_dim_sec` | `15` | `0..3600` (`0` = never auto-dim) |
| `ev` | `0.0` | `-2.0..2.0` |
| `camera_transform` | `"auto"` | `"auto"`, `"0"`, `"180"`, `"hflip"`, `"vflip"` |

---

## 6. CLI Options & Environment Variables

```text
czcamera [options]
  --sim                      Run desktop 320x170 SDL2 simulator (uses mock camera)
  --scale <1..4>             Integer SDL2 window scale factor (default: 2)
  --mock-cam                 Force procedural SimCameraBackend (desktop or device)
  --output-dir <path>        Override media output directory for this session (non-destructive to config.json)
  --render-test <path.ppm>   Render 320x170 P6 PPM snapshot headlessly and exit
  --theme <dark|light>       Initial theme override
  --mode <photo|video|timelapse|gallery|settings>
                             Initial mode or modal
  --rail <right|left>        Control rail side override
  --help                     Print usage help
```

Environment variables:
- `CZCAMERA_CONFIG`: Override path to `config.json`.
- `CZCAMERA_CAM_BIN`: Override path to `czcamera-cam` executable (default `/usr/bin/czcamera-cam`, with `./cam-helper/bin/czcamera-cam` development fallback).

---

## 7. Sync-Safe Storage & Disk Floor

- All in-progress files inside `output_dir` use leading dots so filesystem watchers/sync tools ignore partial writes:
  - Photos: `.IMG_<ts>.jpg.tmp` → atomic `rename()` → `IMG_<ts>.jpg`
  - Videos: `.VID_<ts>.mp4.part` → atomic `rename()` → `VID_<ts>.mp4`
  - Timelapses: `.tl_work_<ts>/frame_0001.jpg` + `manifest.json`, `.TL_<ts>.mp4.part` → `TL_<ts>.mp4` or `TL_<ts>/`
- Minimum free storage floor: **`100 MB` (`104857600` bytes)** checked via `statvfs()` before any capture, every 2s during Video recording, and before each Timelapse shot.

---

## 8. Troubleshooting

- **"Camera Offline [R]" shown on device:**
  1. Check `grep imx219 /boot/firmware/config.txt` (`dtoverlay=imx219`) and `rpicam-hello --list-cameras`.
  2. Verify `/usr/bin/czcamera-cam --mock-driver --frame-fd 3` manually, or press `R` inside `czcamera` to respawn the backend.
- **Video records muted (`audio_unavailable` toast):** Check ALSA capture device visibility (`arecord -l` and `alsamixer`); `czcamera` automatically records muted video rather than dropping the shot.
- **Upside-down or mirrored preview on custom lens mount:** Open **Settings (`S`) → `Camera Orient`** and cycle `auto`, `0`, `180`, `hflip`, or `vflip`.
