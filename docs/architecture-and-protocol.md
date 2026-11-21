> [!IMPORTANT]  
> This project was developed entirely with AI coding assistance (Google Neon) and has not undergone rigorous review. It is provided as-is and may require adjustments for other environments.

# Architecture & Camera Protocol Reference

`czcamera` separates user-space UI, image processing, media assembly, and storage management in C++17 from Raspberry Pi hardware capture in Python 3 (`python3-picamera2`) through an explicit backend abstraction (`cz::ICameraBackend`).

---

## 1. System Topology & Decoupling Boundary

```text
+-------------------------------------------------------------------+
|  C++17 /usr/bin/czcamera                                          |
|  - AppView (320x170 ShadcnCanvas: FreeType2 + 5x7 fallback)       |
|  - UiController, TimelapseEngine, GalleryStore, VideoPlayer       |
|  - Filters RGB24 + libjpeg still pipeline, BacklightController    |
|  - Linux (/dev/fb0 + cp0-keys evdev)  OR  Desktop SDL2 (--sim)    |
|                              |                                    |
|                    cz::ICameraBackend                             |
|              +---------------+---------------+                    |
|              |                               |                    |
|   SubprocessCameraBackend            SimCameraBackend             |
|     (device / --mock-driver)      (--sim / --mock-cam / tests)    |
+--------------|----------------------------------------------------+
               | stdin/stdout JSON v1 + dedicated CZFR fd pipe
               v
+-------------------------------------------------------------------+
|  Python 3 /usr/bin/czcamera-cam (czcamera_cam)                    |
|  - JsonProtocolServer + FramePipeWriter (latest-slot drop-oldest) |
|  - Picam2Worker (main + lores YUV420 dual streams, H264Encoder)   |
|  - MockCameraDriver (--mock-driver deterministic test driver)     |
+-------------------------------------------------------------------+
               | python3-picamera2 / libcamera IPA (rpi/vc4)
               v
       Sony IMX219 (3280x2464) + ES8389 MEMS Mic (ALSA default)
```

### Replacing `picamera2` with Native `libcamera` Later
`czcamera` C++ sources have **zero** `#include <libcamera/...>` or CMake linkage to `libcamera.so`. To swap `python3-picamera2` for a native C++ `libcamera` backend (or `rpicam-apps`), implement [`app/src/camera_backend.hpp`](../app/src/camera_backend.hpp) (`cz::ICameraBackend`) and return it from `create_camera_backend()` in [`app/src/sim_backend.cpp`](../app/src/sim_backend.cpp) without touching `UiController`, `AppView`, filters, timelapse, or gallery code. Interactive launches invoke `ICameraBackend::start()` asynchronously on a worker thread with up to 3 automatic retry attempts (and cancel/defer in-flight startup via `cancel_start()` / `reset_start_cancel()` when opening Gallery or starting `TimelapseRecovery` `[C]` compilation).

---

## 2. Camera Helper Wire Protocol v1

`SubprocessCameraBackend` launches `/usr/bin/czcamera-cam --frame-fd <N>` (overridable via `CZCAMERA_CAM_BIN`, passing `--mock-driver` in protocol tests) with an inherited read/write pipe (`frame_fd`).

### Control Channel (`stdin` / `stdout` Newline-Delimited JSON)

Requests from C++ → Python carry `"id"` (uint64) and `"cmd"`:

| Command | Payload Fields | Reply Extra Fields (`{"id", "ok": true, ...}`) |
|---|---|---|
| `configure` | `mode` (`"photo"`/`"video"`/`"timelapse"`), `main_w`, `main_h`, `preview_w` (`226`), `preview_h` (`170`), `fps`, `zoom`, `ev`, `transform` (`"auto"`/`"0"`/`"180"`/`"hflip"`/`"vflip"`) | `mode`, `main_w`, `main_h`, `preview_w`, `preview_h` (`226x128` for 16:9 modes), `transform`, `is_16_9` |
| `set_controls` | Optional `zoom` (`1.0..4.0`), `ev` (`-2.0..+2.0`) | `zoom`, `ev` |
| `capture_still` | `tmp_path`, `width`, `height` | `width`, `height`, `bytes` (encodes `YUV420` via `simplejpeg` directly from `main` for `2MP`/`1080p`, or temporarily switches to a `buffer_count=1` `YUV420` still config for `8MP`/Video and restores stream config) |
| `start_video` | `tmp_path`, `width`, `height`, `fps`, `bitrate`, `audio` (`bool`) | `audio_recorded` (`bool`) |
| `stop_video` | — | `duration_sec`, `bytes`, `audio_recorded` |
| `pause` | — | Stops recording if active, closes camera & releases IMX219 lock |
| `resume` | — | Re-opens camera and restores last `configure` spec |
| `shutdown` | — | Tears down writer & camera cleanly and exits `0` |

Asynchronous Events from Python → C++ (no `"id"`):
- `{"event": "ready", "protocol": 1, "backend": "picamera2"|"mock", "sensor": "...", "has_audio": true|false}`
- `{"event": "warn", "code": "...", "message": "..."}` (for example `"audio_unavailable"`, `"invalid_json"`, `"control_error"`)
- `{"event": "error", "code": "...", "message": "..."}` (for example `"startup_failed"` emitted by `JsonProtocolServer` when worker `start()` raises, with the traceback written to stderr / `~/.local/share/czcamera/czcamera.log`)

### Binary Preview Frame Channel (`--frame-fd <N>`, 16-Byte `CZFR` Header)

Every preview frame packet starts with a fixed **16-byte little-endian** header (`struct.Struct("<4sHHBBHI")`) followed immediately by `payload_len` bytes:

| Offset | Size | C Type | Field | Description |
|---|---|---|---|---|
| `0` | `4` | `char[4]` | `magic` | `"CZFR"` (`0x43 0x5A 0x46 0x52`) |
| `4` | `2` | `uint16_t` | `width` | Negotiated preview frame width (e.g., `226`) |
| `6` | `2` | `uint16_t` | `height` | Preview frame height (`170` for 4:3, `128` for 16:9) |
| `8` | `1` | `uint8_t` | `pixfmt` | `1` = `YUV420`, `2` = `RGB24` |
| `9` | `1` | `uint8_t` | `flags` | Bit `0` (`0x01`): `FLAG_ASPECT_16_9` |
| `10` | `2` | `uint16_t` | `reserved` | Always `0` |
| `12` | `4` | `uint32_t` | `payload_len` | `width * height * 3 / 2` (`YUV420`) or `width * height * 3` (`RGB24`) |

**Backpressure Contract (Review Focus #1):**
- `FramePipeWriter` uses a single-slot bounded queue (`_pending`). When the UI consumer slows down, `push_frame()` drops any unconsumed queued packet and replaces it with the newest frame, keeping camera/H.264 encoder threads non-blocking.
- `Picam2Worker._extract_lores_yuv420()` inspects the negotiated `lores` `stride` (handling 64-byte DMA padding such as `256` for `226px`) and strips row padding before packing tight `YUV420` payloads.

---

## 3. Target Hardware Constraints & Media Pipelines

- **Sony IMX219 Sensor Modes:** `640×480` (4:3, crop `(1000,752)/1280×960`), `1640×1232` (4:3 full-FOV 2×2 binned), `1920×1080` (16:9 crop), `3280×2464` (4:3 8MP full sensor).
- **Low-CMA Camera Streams (`Picam2Worker`):** Photo and Timelapse modes run live preview with `create_preview_configuration` (`main` `YUV420` at `1640×1232` for `4:3` or `1920×1080` for `16:9`, `lores` `YUV420`, `buffer_count=2`, `raw=None`). Video mode uses `create_video_configuration` (`buffer_count=4`, `raw=None`). Stills encode `YUV420` planes to JPEG via `simplejpeg` (`2MP`/`1080p` fast-path without mode switching; `8MP` or Video-mode stills stop preview for an on-demand `buffer_count=1` `YUV420` still capture and restore preview afterward).
- **Video Output (`FfmpegOutput`):** `_FfmpegOutputTarget` overrides `.split()` cleanly around `picamera2.outputs.FfmpegOutput`'s whitespace `.split()` so staging paths containing spaces or special characters (and ending in `.mp4.part`, with `-f mp4 -movflags +faststart`) remain exact `argv` tokens.
- **Timelapse Compilation:** [`TimelapseEngine`](../app/src/timelapse_engine.hpp) invokes `ffmpeg` with `-threads 1 -filter_threads 1` and scales `4:3` (`2mp`/`8mp`) to `1024:768` or `16:9` (`1080p`) to `1280:720` (`bilinear,setsar=1`) to stay safely within CardputerZero's 512 MB RAM / 64 MB CMA budget and VideoCore IV `/dev/video11` constraints, encoding with `h264_v4l2m2m` (`-num_output_buffers 4`, `5M`, `yuv420p`) and falling back to low-memory `libx264` (`-threads 2 -preset ultrafast -crf 23 -x264-params rc-lookahead=0:ref=1:bframes=0:threads=2:lookahead-threads=1:sync-lookahead=0`) on failure.
- **Gallery Playback:** [`VideoPlayer`](../app/src/video_player.hpp) probes duration & audio presence via `ffprobe`, spawns `ffmpeg` raw `rgb24` frames timed cleanly to PTS, and drives audio through ALSA `aplay` on Linux or `set_pcm_sink` (SDL2 audio queue) on desktop—allowing silent `.mp4` files (compiled timelapses or muted clips) to play smoothly without audio failure.

---

## 4. `320×170` UI Renderer & Zinc Color Tokens

[`AppView`](../app/src/app_ui.hpp) renders into an offscreen `320×170` ARGB8888 canvas with SDF antialiased rounded primitives and FreeType2 + `5×7` bitmap fallback, then blits via [`fb_blit.hpp`](../app/src/fb_blit.hpp) (`RGB565` / `RGB24` / `XRGB8888`) or SDL2 texture.

Exact palette tokens defined in [`app/src/theme.cpp`](../app/src/theme.cpp):

| Token | Zinc Dark (`ThemeMode::Dark`) | Zinc Light (`ThemeMode::Light`) |
|---|---|---|
| `background` / `card` | `#09090b` / `#09090b` | `#fafafa` / `#ffffff` |
| `muted` | `#18181b` | `#f4f4f5` |
| `border` | `#27272a` | `#e4e4e7` |
| `ring` | `#d4d4d8` | `#71717a` |
| `foreground` / `muted_foreground` | `#fafafa` / `#a1a1aa` | `#09090b` / `#71717a` |
| `active_tab` / `active_tab_foreground` | `#27272a` / `#fafafa` | `#ffffff` / `#09090b` |
| `status_online` / `status_online_bg` | `#4ade80` / `#11261a` | `#16a34a` / `#dcfce7` |
| `status_sleeping` / `status_sleeping_bg` | `#fbbf24` / `#271d0b` | `#d97706` / `#fef3c7` |
| `status_error` / `status_error_bg` | `#f87171` / `#2b1214` | `#dc2626` / `#fee2e2` |
| `viewfinder_surround` | `#050507` | `#050507` |

---

## 5. Source Directory Map

- **`app/src/`**:
  - `theme.{hpp,cpp}`, `fb_blit.hpp`, `config.{hpp,cpp}`, `storage.{hpp,cpp}`
  - `filters.{hpp,cpp}`, `jpeg_pipeline.{hpp,cpp}`
  - `camera_backend.hpp`, `subprocess_backend.{hpp,cpp}`, `sim_backend.{hpp,cpp}`
  - `backlight.{hpp,cpp}`, `timelapse_engine.{hpp,cpp}`, `gallery_model.{hpp,cpp}`, `video_player.{hpp,cpp}`
  - `ui_controller.{hpp,cpp}`, `app_ui.{hpp,cpp}`, `evdev_repeat_guard.hpp`, `main.cpp`
- **`app/tests/`**: `test_config_and_storage.cpp`, `test_filters_and_jpeg.cpp`, `test_backend_protocol.cpp`, `test_timelapse_and_gallery.cpp`, `test_ui_controller.cpp`
- **`cam-helper/`**: `bin/czcamera-cam`, `czcamera_cam/{__init__,protocol,picam2_worker}.py`, `tests/test_{protocol,picam2_worker}.py`
- **`packaging/`**: `Dockerfile.arm64`, `build-deb.sh`, `czcamera.desktop`, `czcamera.png`, `debian/{control,postinst,prerm}`, `tests/test_packaging.py`
- **`docs/`**: User/developer guides (`building-and-installation.md`, `usage-and-controls.md`, `architecture-and-protocol.md`)
- **`LICENSE.md`**: Apache License, Version 2.0
