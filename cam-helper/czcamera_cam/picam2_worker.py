"""Picamera2 dual-stream worker and MockCameraDriver for czcamera-cam."""

from __future__ import annotations

import os
import struct
import threading
import time
from typing import Any, Callable, Optional

from .protocol import (
    FLAG_ASPECT_16_9,
    PIXFMT_YUV420,
    PROTOCOL_VERSION,
    FramePipeWriter,
)

# Standard JPEG Annex K Huffman tables for 3-component baseline DCT JPEGs.
_DC_LUM_BITS = bytes([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0])
_DC_LUM_VALS = bytes([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11])
_DC_CHROM_BITS = bytes([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0])
_DC_CHROM_VALS = bytes([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11])

_AC_LUM_BITS = bytes([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D])
_AC_LUM_VALS = bytes(
    [
        0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
        0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
        0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
        0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
        0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
        0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
        0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
        0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
        0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
        0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
        0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4,
        0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
    ]
)

_AC_CHROM_BITS = bytes([0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77])
_AC_CHROM_VALS = bytes(
    [
        0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
        0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
        0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1,
        0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
        0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44,
        0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
        0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74,
        0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
        0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
        0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
        0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
        0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
        0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4,
        0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
    ]
)

# 4 consecutive solid-color MCUs (zero DC diff, EOB AC) pack cleanly into 7 bytes
# without any 0xFF bytes: 4 * (00 1010 00 00 00 00) = 56 bits.
_FOUR_ZERO_MCUS = bytes([0x28, 0x00, 0xA0, 0x02, 0x80, 0x0A, 0x00])


def compute_scaler_crop(
    pixel_array_size: tuple[int, int],
    aspect_w: int,
    aspect_h: int,
    zoom: float,
) -> tuple[int, int, int, int]:
    """Compute even-aligned ScalerCrop (x, y, w, h) centered within sensor bounds."""
    sensor_w, sensor_h = int(pixel_array_size[0]), int(pixel_array_size[1])
    if sensor_w < 2 or sensor_h < 2:
        raise ValueError(f"Invalid pixel_array_size: {pixel_array_size}")
    if aspect_w <= 0 or aspect_h <= 0:
        raise ValueError(f"Invalid aspect ratio: {aspect_w}:{aspect_h}")

    z = max(1.0, float(zoom))
    sensor_aspect = sensor_w / sensor_h
    target_aspect = aspect_w / aspect_h
    if abs(target_aspect - sensor_aspect) < 0.01:
        base_w = float(sensor_w)
        base_h = float(sensor_h)
    elif aspect_w * sensor_h >= aspect_h * sensor_w:
        base_w = float(sensor_w)
        base_h = sensor_w * (aspect_h / aspect_w)
    else:
        base_h = float(sensor_h)
        base_w = sensor_h * (aspect_w / aspect_h)

    crop_w = max(2, int(base_w / z) & ~1)
    crop_h = max(2, int(base_h / z) & ~1)
    crop_w = min(crop_w, sensor_w & ~1)
    crop_h = min(crop_h, sensor_h & ~1)

    x = max(0, ((sensor_w - crop_w) // 2) & ~1)
    y = max(0, ((sensor_h - crop_h) // 2) & ~1)
    return (x, y, crop_w, crop_h)


def resolve_transform(
    transform_str: str,
    camera_properties: Optional[dict[str, Any]],
    transform_cls: Any = None,
) -> Any:
    """Resolve Wire Protocol v1 transform string into a libcamera.Transform."""
    if transform_cls is None:
        from libcamera import Transform  # type: ignore[import-not-found]

        transform_cls = Transform

    key = (transform_str or "auto").strip().lower()
    if key == "auto":
        rotation = int((camera_properties or {}).get("Rotation", 0))
        if rotation == 180:
            hflip, vflip = 1, 1
        else:
            hflip, vflip = 0, 0
    elif key == "0":
        hflip, vflip = 0, 0
    elif key == "180":
        hflip, vflip = 1, 1
    elif key == "hflip":
        hflip, vflip = 1, 0
    elif key == "vflip":
        hflip, vflip = 0, 1
    else:
        raise ValueError(f"Unsupported transform: {transform_str}")

    return transform_cls(hflip=hflip, vflip=vflip)


def _is_16_9_mode(mode: str, main_w: int, main_h: int) -> bool:
    if mode == "video":
        return True
    if main_h > 0 and abs((main_w / main_h) - (16.0 / 9.0)) < 0.05:
        return True
    return False


class _FfmpegOutputTarget(str):
    """str subclass for picamera2.outputs.FfmpegOutput preserving path tokens.

    Upstream FfmpegOutput.start() parses output_filename with plain str.split()
    and no shell, so paths containing spaces or shlex quotes would break.
    """

    _tokens: tuple[str, ...]

    def __new__(cls, *tokens: str) -> "_FfmpegOutputTarget":
        obj = super().__new__(cls, " ".join(tokens))
        obj._tokens = tuple(tokens)
        return obj

    def split(self, sep: Optional[str] = None, maxsplit: int = -1) -> list[str]:
        if sep is None and maxsplit == -1:
            return list(self._tokens)
        return super().split(sep, maxsplit)


def _probe_alsa_available() -> bool:
    cards_path = "/proc/asound/cards"
    if not os.path.exists(cards_path):
        return False
    try:
        with open(cards_path, "r", encoding="utf-8", errors="ignore") as f:
            text = f.read()
        return bool(text.strip()) and "no soundcards" not in text.lower()
    except OSError:
        return False


def _write_minimal_jpeg(path: str, width: int, height: int, ev: float = 0.0) -> int:
    """Write a valid baseline 3-component JFIF JPEG readable by libjpeg."""
    w = max(1, min(65535, int(width)))
    h = max(1, min(65535, int(height)))

    def dht_segment(tc_th: int, bits: bytes, vals: bytes) -> bytes:
        payload = bytes([tc_th]) + bits + vals
        return b"\xff\xc4" + struct.pack(">H", len(payload) + 2) + payload

    # Quantization tables (uniform 8s)
    qtable = bytes([8] * 64)
    dqt0 = b"\xff\xdb" + struct.pack(">H", 67) + b"\x00" + qtable
    dqt1 = b"\xff\xdb" + struct.pack(">H", 67) + b"\x01" + qtable

    # JFIF APP0
    app0 = (
        b"\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
    )

    # SOF0 (Baseline DCT, 8-bit, 3 components 1x1 4:4:4)
    sof0 = (
        b"\xff\xc0\x00\x11\x08"
        + struct.pack(">HH", h, w)
        + b"\x03\x01\x11\x00\x02\x11\x01\x03\x11\x01"
    )

    dht = (
        dht_segment(0x00, _DC_LUM_BITS, _DC_LUM_VALS)
        + dht_segment(0x01, _DC_CHROM_BITS, _DC_CHROM_VALS)
        + dht_segment(0x10, _AC_LUM_BITS, _AC_LUM_VALS)
        + dht_segment(0x11, _AC_CHROM_BITS, _AC_CHROM_VALS)
    )

    sos = b"\xff\xda\x00\x0c\x03\x01\x00\x02\x11\x03\x11\x00\x3f\x00"

    # Encode MCUs: 8x8 blocks in 4:4:4
    mcu_cols = (w + 7) // 8
    mcu_rows = (h + 7) // 8
    total_mcus = mcu_cols * mcu_rows

    # Shift Y DC slightly by EV while keeping all remaining MCUs zero-diff
    # (For simplicity and speed, use DC diff 0 when EV == 0, or a small valid DC diff).
    dc_y_delta = max(-60, min(60, int(round(ev * 25.0))))
    scan = bytearray()
    bit_buf = 0
    bit_count = 0

    def emit_bits(val: int, length: int) -> None:
        nonlocal bit_buf, bit_count
        bit_buf = (bit_buf << length) | (val & ((1 << length) - 1))
        bit_count += length
        while bit_count >= 8:
            bit_count -= 8
            byte = (bit_buf >> bit_count) & 0xFF
            scan.append(byte)
            if byte == 0xFF:
                scan.append(0x00)
            bit_buf &= (1 << bit_count) - 1

    # First MCU: Y DC diff (shifted by ev), AC EOB (1010); Cb/Cr DC=0 (00), AC EOB (00)
    if dc_y_delta == 0:
        emit_bits(0b00, 2)
    else:
        mag = abs(dc_y_delta)
        cat = mag.bit_length()
        # Standard DC lum Huffman codes for categories 1..8
        dc_lum_codes = {
            1: (0b010, 3),
            2: (0b011, 3),
            3: (0b100, 3),
            4: (0b101, 3),
            5: (0b110, 3),
            6: (0b1110, 4),
            7: (0b11110, 5),
            8: (0b111110, 6),
        }
        code, clen = dc_lum_codes[cat]
        emit_bits(code, clen)
        bits_val = dc_y_delta if dc_y_delta > 0 else ((1 << cat) - 1 + dc_y_delta)
        emit_bits(bits_val, cat)
    emit_bits(0b1010, 4)  # Y AC EOB
    emit_bits(0b00, 2)    # Cb DC diff 0
    emit_bits(0b00, 2)    # Cb AC EOB
    emit_bits(0b00, 2)    # Cr DC diff 0
    emit_bits(0b00, 2)    # Cr AC EOB

    # Remaining MCUs are all 14-bit zero-diff + EOB (`00 1010 00 00 00 00`).
    rem = total_mcus - 1
    if bit_count == 0 and rem >= 4:
        chunks = rem // 4
        scan.extend(_FOUR_ZERO_MCUS * chunks)
        rem %= 4

    for _ in range(rem):
        emit_bits(0b00101000000000, 14)

    if bit_count > 0:
        pad = 8 - bit_count
        emit_bits((1 << pad) - 1, pad)

    jpeg_bytes = b"\xff\xd8" + app0 + dqt0 + dqt1 + sof0 + dht + sos + bytes(scan) + b"\xff\xd9"
    with open(path, "wb") as f:
        f.write(jpeg_bytes)
        f.flush()
    return len(jpeg_bytes)


def _write_minimal_mp4_stub(path: str) -> int:
    """Write a minimal ISO BMFF MP4 header stub for mock video testing."""
    ftyp = b"\x00\x00\x00\x18ftypmp42\x00\x00\x00\x00mp42isom"
    free = b"\x00\x00\x00\x08free"
    mdat = b"\x00\x00\x00\x10mdat\x00\x00\x00\x00\x00\x00\x00\x00"
    data = ftyp + free + mdat
    with open(path, "wb") as f:
        f.write(data)
        f.flush()
    return len(data)


def _read_stream_buffer(request: Any, stream_name: str) -> Any:
    if hasattr(request, "make_buffer"):
        return request.make_buffer(stream_name)
    return request.make_array(stream_name).tobytes()


def _clone_config_dict(config: Any) -> Any:
    if not isinstance(config, dict):
        return config
    cloned = dict(config)
    for key, val in cloned.items():
        if isinstance(val, dict):
            cloned[key] = dict(val)
    return cloned


def extract_yuv420_planes(
    raw: bytes | bytearray | memoryview,
    width: int,
    height: int,
    stride: Optional[int] = None,
) -> tuple[bytes, bytes, bytes]:
    """Extract tight (Y, U, V) byte planes from a possibly strided YUV420 buffer."""
    w = int(width)
    h = int(height)
    if w < 2 or h < 2 or (w & 1) or (h & 1):
        raise ValueError(f"Invalid YUV420 dimensions: {w}x{h}")

    y_len = w * h
    uv_w = w // 2
    uv_h = h // 2
    uv_len = uv_w * uv_h
    expected = y_len + 2 * uv_len

    mv = memoryview(raw).cast("B")
    try:
        buf_len = len(mv)
        if buf_len < expected:
            raise ValueError(
                f"YUV420 buffer too small for {w}x{h}: got {buf_len} bytes, need {expected}"
            )

        if stride is not None and stride > 0:
            effective_stride = int(stride)
        elif buf_len == expected:
            effective_stride = w
        else:
            total_rows = h + uv_h
            if total_rows > 0 and (buf_len % total_rows) == 0:
                candidate = buf_len // total_rows
                effective_stride = (
                    candidate if candidate >= w and (candidate % 2) == 0 else w
                )
            else:
                effective_stride = w

        if effective_stride <= w:
            return (
                bytes(mv[:y_len]),
                bytes(mv[y_len : y_len + uv_len]),
                bytes(mv[y_len + uv_len : expected]),
            )

        uv_stride = effective_stride // 2
        u_base = h * effective_stride
        v_base = u_base + uv_h * uv_stride
        required_strided = v_base + uv_h * uv_stride
        if buf_len < required_strided:
            raise ValueError(
                f"YUV420 buffer too small for {w}x{h} with stride {effective_stride}: "
                f"got {buf_len} bytes, need {required_strided}"
            )

        try:
            import numpy as np  # type: ignore[import-not-found]
        except ImportError:
            np = None

        if np is not None:
            arr = None
            try:
                arr = np.frombuffer(mv, dtype=np.uint8)
                y_bytes = arr[:u_base].reshape(h, effective_stride)[:, :w].tobytes()
                u_bytes = (
                    arr[u_base:v_base].reshape(uv_h, uv_stride)[:, :uv_w].tobytes()
                )
                v_bytes = (
                    arr[v_base:required_strided]
                    .reshape(uv_h, uv_stride)[:, :uv_w]
                    .tobytes()
                )
                return y_bytes, u_bytes, v_bytes
            finally:
                arr = None

        y_out = bytearray(y_len)
        for r in range(h):
            row_start = r * effective_stride
            y_out[r * w : (r + 1) * w] = mv[row_start : row_start + w]

        u_out = bytearray(uv_len)
        v_out = bytearray(uv_len)
        for r in range(uv_h):
            u_start = u_base + r * uv_stride
            v_start = v_base + r * uv_stride
            dst = r * uv_w
            u_out[dst : dst + uv_w] = mv[u_start : u_start + uv_w]
            v_out[dst : dst + uv_w] = mv[v_start : v_start + uv_w]

        return bytes(y_out), bytes(u_out), bytes(v_out)
    finally:
        mv.release()


def _default_yuv420_jpeg_encoder(
    y_bytes: bytes,
    u_bytes: bytes,
    v_bytes: bytes,
    width: int,
    height: int,
    quality: int,
) -> bytes:
    """Encode tight YUV420 planes to JPEG bytes using simplejpeg."""
    import numpy as np  # type: ignore[import-not-found]
    import simplejpeg  # type: ignore[import-not-found]

    y_2d = np.ascontiguousarray(
        np.frombuffer(y_bytes, dtype=np.uint8).reshape(height, width)
    )
    u_2d = np.ascontiguousarray(
        np.frombuffer(u_bytes, dtype=np.uint8).reshape(height // 2, width // 2)
    )
    v_2d = np.ascontiguousarray(
        np.frombuffer(v_bytes, dtype=np.uint8).reshape(height // 2, width // 2)
    )
    return bytes(
        simplejpeg.encode_jpeg_yuv_planes(y_2d, u_2d, v_2d, quality=int(quality))
    )


def encode_yuv420_to_jpeg(
    raw: bytes | bytearray | memoryview,
    width: int,
    height: int,
    stride: Optional[int],
    tmp_path: str,
    quality: int = 92,
    encoder: Optional[Callable[[bytes, bytes, bytes, int, int, int], bytes]] = None,
) -> int:
    """Strip stride padding from YUV420 raw buffer, encode JPEG, and flush to tmp_path."""
    y_bytes, u_bytes, v_bytes = extract_yuv420_planes(raw, width, height, stride=stride)
    encode_fn = encoder if encoder is not None else _default_yuv420_jpeg_encoder
    jpeg_bytes = encode_fn(y_bytes, u_bytes, v_bytes, width, height, int(quality))
    with open(tmp_path, "wb") as f:
        f.write(jpeg_bytes)
        f.flush()
    return len(jpeg_bytes)


class Picam2Worker:
    """Real Picamera2 dual-stream (main + lores) worker."""

    def __init__(
        self,
        frame_writer: FramePipeWriter,
        emit_event: Callable[[dict[str, Any]], None],
        picamera2_cls: Any = None,
        transform_cls: Any = None,
        encoder_cls: Any = None,
        ffmpeg_output_cls: Any = None,
        audio_probe: Optional[Callable[[], bool]] = None,
        yuv_jpeg_encoder: Optional[
            Callable[[bytes, bytes, bytes, int, int, int], bytes]
        ] = None,
    ) -> None:
        self._frame_writer = frame_writer
        self._emit_event = emit_event
        self._picamera2_cls = picamera2_cls
        self._transform_cls = transform_cls
        self._encoder_cls = encoder_cls
        self._ffmpeg_output_cls = ffmpeg_output_cls
        self._audio_probe = audio_probe or _probe_alsa_available
        self._yuv_jpeg_encoder = yuv_jpeg_encoder

        self._picam2: Any = None
        self._camera_properties: dict[str, Any] = {}
        self._sensor_size: tuple[int, int] = (3280, 2464)
        self._state_lock = threading.Lock()
        self._started = False
        self._paused = False
        self._recording = False

        self._mode = "photo"
        self._main_size = (3280, 2464)
        self._active_main_size: Optional[tuple[int, int]] = None
        self._preview_size = (226, 170)
        self._lores_stride: Optional[int] = None
        self._main_stride: Optional[int] = None
        self._fps = 30
        self._zoom = 1.0
        self._ev = 0.0
        self._transform_str = "auto"
        self._is_16_9 = False
        self._active_config: Any = None
        self._last_config_kwargs: Optional[dict[str, Any]] = None

        self._video_tmp_path = ""
        self._video_start_monotonic = 0.0
        self._audio_recorded = False

    def _ensure_picamera2_imports(self) -> None:
        if self._picamera2_cls is None:
            from picamera2 import Picamera2  # type: ignore[import-not-found]

            self._picamera2_cls = Picamera2
        if self._transform_cls is None:
            from libcamera import Transform  # type: ignore[import-not-found]

            self._transform_cls = Transform
        if self._encoder_cls is None:
            from picamera2.encoders import H264Encoder  # type: ignore[import-not-found]

            self._encoder_cls = H264Encoder
        if self._ffmpeg_output_cls is None:
            from picamera2.outputs import FfmpegOutput  # type: ignore[import-not-found]

            self._ffmpeg_output_cls = FfmpegOutput

    def _open_camera(self) -> None:
        self._ensure_picamera2_imports()
        self._picam2 = self._picamera2_cls()
        self._camera_properties = dict(getattr(self._picam2, "camera_properties", {}) or {})
        raw_size = self._camera_properties.get("PixelArraySize", (3280, 2464))
        self._sensor_size = (int(raw_size[0]), int(raw_size[1]))

    def start(self) -> None:
        self._open_camera()
        sensor = str(self._camera_properties.get("Model", "imx219"))
        has_audio = bool(self._audio_probe())
        self._emit_event(
            {
                "event": "ready",
                "protocol": PROTOCOL_VERSION,
                "backend": "picamera2",
                "sensor": sensor,
                "has_audio": has_audio,
            }
        )

    def _read_negotiated_stream_stride(
        self, stream_name: str, request: Any = None
    ) -> Optional[int]:
        if request is not None:
            req_cfg = getattr(request, "config", None)
            if isinstance(req_cfg, dict):
                stream_cfg = req_cfg.get(stream_name)
                if (
                    isinstance(stream_cfg, dict)
                    and isinstance(stream_cfg.get("stride"), int)
                    and stream_cfg["stride"] > 0
                ):
                    return int(stream_cfg["stride"])
        if self._picam2 is not None:
            stream_cfg_fn = getattr(self._picam2, "stream_configuration", None)
            if callable(stream_cfg_fn):
                try:
                    stream_cfg = stream_cfg_fn(stream_name)
                    if (
                        isinstance(stream_cfg, dict)
                        and isinstance(stream_cfg.get("stride"), int)
                        and stream_cfg["stride"] > 0
                    ):
                        return int(stream_cfg["stride"])
                except Exception:
                    pass
            cam_cfg = getattr(self._picam2, "camera_config", None)
            if isinstance(cam_cfg, dict):
                stream_cfg = cam_cfg.get(stream_name)
                if (
                    isinstance(stream_cfg, dict)
                    and isinstance(stream_cfg.get("stride"), int)
                    and stream_cfg["stride"] > 0
                ):
                    return int(stream_cfg["stride"])
        return None

    def _extract_lores_yuv420(
        self,
        request: Any,
        width: int,
        height: int,
        stride_hint: Optional[int] = None,
    ) -> bytes:
        raw = _read_stream_buffer(request, "lores")
        stride = self._read_negotiated_stream_stride("lores", request) or stride_hint
        y_plane, u_plane, v_plane = extract_yuv420_planes(
            raw, width, height, stride=stride
        )
        return y_plane + u_plane + v_plane

    def _on_request(self, request: Any) -> None:
        with self._state_lock:
            if self._paused or not self._started:
                return
            pw, ph = self._preview_size
            is_16_9 = self._is_16_9
            stride_hint = self._lores_stride
        try:
            payload = self._extract_lores_yuv420(request, pw, ph, stride_hint=stride_hint)
        except Exception:
            return
        flags = FLAG_ASPECT_16_9 if is_16_9 else 0
        self._frame_writer.push_frame(pw, ph, PIXFMT_YUV420, flags, payload)

    def _current_scaler_crop(self) -> tuple[int, int, int, int]:
        aspect = (16, 9) if self._is_16_9 else (4, 3)
        return compute_scaler_crop(self._sensor_size, aspect[0], aspect[1], self._zoom)

    def _build_active_config(
        self,
        mode: str,
        mw: int,
        mh: int,
        pw: int,
        ph: int,
        req_fps: int,
        transform_obj: Any,
    ) -> tuple[Any, tuple[int, int]]:
        lores_cfg = {"size": (pw, ph), "format": "YUV420"}
        if mode == "video":
            active_size = (mw, mh)
            config = self._picam2.create_video_configuration(
                main={"size": active_size, "format": "YUV420"},
                lores=lores_cfg,
                raw=None,
                buffer_count=4,
                transform=transform_obj,
                controls={"FrameRate": req_fps},
            )
        else:
            active_size = (1920, 1080) if _is_16_9_mode(mode, mw, mh) else (1640, 1232)
            config = self._picam2.create_preview_configuration(
                main={"size": active_size, "format": "YUV420"},
                lores=lores_cfg,
                raw=None,
                buffer_count=2,
                transform=transform_obj,
                controls={"FrameRate": req_fps},
            )
        return config, active_size

    def configure(
        self,
        mode: str,
        main_w: int,
        main_h: int,
        preview_w: int,
        preview_h: int,
        fps: int,
        zoom: float,
        ev: float,
        transform: str,
    ) -> dict[str, Any]:
        if self._recording:
            raise RuntimeError("camera_recording")
        if mode not in ("photo", "video", "timelapse"):
            raise ValueError(f"Unsupported mode: {mode}")

        pw = max(2, int(preview_w) & ~1)
        ph = max(2, int(preview_h) & ~1)
        mw = max(2, int(main_w) & ~1)
        mh = max(2, int(main_h) & ~1)
        req_fps = max(1, int(fps))
        req_zoom = max(1.0, min(4.0, float(zoom)))
        req_ev = max(-2.0, min(2.0, float(ev)))

        if self._picam2 is None:
            self._open_camera()

        # Validate transform before stopping camera or mutating worker state.
        transform_obj = resolve_transform(
            transform,
            self._camera_properties,
            transform_cls=self._transform_cls,
        )

        # Detach callback and stop camera before mutating shared geometry/state.
        if self._started:
            self._picam2.post_callback = None
            self._picam2.stop()
            with self._state_lock:
                self._started = False

        config, active_main_size = self._build_active_config(
            mode, mw, mh, pw, ph, req_fps, transform_obj
        )
        saved_config = _clone_config_dict(config)
        self._picam2.configure(config)
        negotiated_lores_stride = self._read_negotiated_stream_stride("lores")
        negotiated_main_stride = self._read_negotiated_stream_stride("main")
        is_16_9 = _is_16_9_mode(mode, mw, mh)

        with self._state_lock:
            self._mode = mode
            self._main_size = (mw, mh)
            self._active_main_size = active_main_size
            self._preview_size = (pw, ph)
            self._lores_stride = negotiated_lores_stride
            self._main_stride = negotiated_main_stride
            self._fps = req_fps
            self._transform_str = transform
            self._is_16_9 = is_16_9
            self._active_config = saved_config
            self._last_config_kwargs = {
                "mode": mode,
                "main_w": mw,
                "main_h": mh,
                "preview_w": pw,
                "preview_h": ph,
                "fps": req_fps,
                "zoom": req_zoom,
                "ev": req_ev,
                "transform": transform,
            }
            self._paused = False

        self._start_camera_with_callback()
        self.set_controls(zoom=req_zoom, ev=req_ev)
        return {
            "mode": self._mode,
            "main_w": mw,
            "main_h": mh,
            "preview_w": pw,
            "preview_h": ph,
        }

    def _start_camera_with_callback(self) -> None:
        with self._state_lock:
            self._started = True
        self._picam2.post_callback = self._on_request
        try:
            self._picam2.start()
        except Exception:
            with self._state_lock:
                self._started = False
            self._picam2.post_callback = None
            raise

    def _capture_main_yuv420_to_jpeg(
        self,
        tmp_path: str,
        width: int,
        height: int,
        stride_fallback: Optional[int] = None,
    ) -> None:
        request = None
        raw = None
        try:
            request = self._picam2.capture_request()
            raw = _read_stream_buffer(request, "main")
            stride = (
                self._read_negotiated_stream_stride("main", request)
                or stride_fallback
            )
            encode_yuv420_to_jpeg(
                raw,
                width,
                height,
                stride,
                tmp_path,
                encoder=self._yuv_jpeg_encoder,
            )
        finally:
            raw = None
            if request is not None:
                request.release()

    def set_controls(
        self,
        zoom: Optional[float] = None,
        ev: Optional[float] = None,
    ) -> dict[str, Any]:
        if zoom is not None:
            self._zoom = max(1.0, min(4.0, float(zoom)))
        if ev is not None:
            self._ev = max(-2.0, min(2.0, float(ev)))

        if self._last_config_kwargs is not None:
            self._last_config_kwargs["zoom"] = self._zoom
            self._last_config_kwargs["ev"] = self._ev

        if self._picam2 is not None and self._started:
            self._picam2.set_controls(
                {
                    "ScalerCrop": self._current_scaler_crop(),
                    "ExposureValue": float(self._ev),
                }
            )
        return {"zoom": self._zoom, "ev": self._ev}

    def capture_still(self, tmp_path: str, width: int, height: int) -> dict[str, Any]:
        if self._picam2 is None or not self._started:
            raise RuntimeError("camera_not_started")
        if self._recording:
            raise RuntimeError("camera_busy")

        req_w = max(2, int(width) & ~1)
        req_h = max(2, int(height) & ~1)
        use_fast_path = (
            self._mode != "video" and (req_w, req_h) == self._active_main_size
        )

        if use_fast_path:
            self._capture_main_yuv420_to_jpeg(
                tmp_path, req_w, req_h, stride_fallback=self._main_stride
            )
        else:
            transform_obj = resolve_transform(
                self._transform_str,
                self._camera_properties,
                transform_cls=self._transform_cls,
            )
            crop_aspect = (
                (16, 9) if _is_16_9_mode(self._mode, req_w, req_h) else (4, 3)
            )
            still_controls = {
                "ScalerCrop": compute_scaler_crop(
                    self._sensor_size, crop_aspect[0], crop_aspect[1], self._zoom
                ),
                "ExposureValue": float(self._ev),
            }
            # Retain Picamera2 default raw={} per Spec §5.1 / rpi_apps.yaml (raw_mandatory_stream: true).
            still_cfg = self._picam2.create_still_configuration(
                main={"size": (req_w, req_h), "format": "YUV420"},
                lores=None,
                buffer_count=1,
                transform=transform_obj,
                controls=still_controls,
            )
            prior_config = _clone_config_dict(self._active_config)
            if prior_config is None:
                prior_config, _ = self._build_active_config(
                    self._mode,
                    self._main_size[0],
                    self._main_size[1],
                    self._preview_size[0],
                    self._preview_size[1],
                    self._fps,
                    transform_obj,
                )

            self._picam2.post_callback = None
            self._picam2.stop()
            with self._state_lock:
                self._started = False

            still_started = False
            try:
                self._picam2.configure(still_cfg)
                self._picam2.start()
                still_started = True
                self._capture_main_yuv420_to_jpeg(
                    tmp_path, req_w, req_h, stride_fallback=None
                )
            finally:
                if still_started:
                    self._picam2.stop()
                self._picam2.configure(_clone_config_dict(prior_config))
                self._lores_stride = (
                    self._read_negotiated_stream_stride("lores") or self._lores_stride
                )
                self._main_stride = (
                    self._read_negotiated_stream_stride("main") or self._main_stride
                )
                self._start_camera_with_callback()
                self.set_controls()

        file_bytes = os.path.getsize(tmp_path) if os.path.exists(tmp_path) else 0
        return {"width": req_w, "height": req_h, "bytes": file_bytes}

    def start_video(
        self,
        tmp_path: str,
        width: int,
        height: int,
        fps: int,
        bitrate: int,
        audio: bool,
    ) -> dict[str, Any]:
        if self._picam2 is None or self._paused:
            raise RuntimeError("camera_not_started")
        if self._recording:
            raise RuntimeError("already_recording")

        req_w = max(2, int(width) & ~1)
        req_h = max(2, int(height) & ~1)
        req_fps = max(1, int(fps))

        if (
            not self._started
            or self._mode != "video"
            or self._main_size != (req_w, req_h)
            or self._fps != req_fps
        ):
            self.configure(
                mode="video",
                main_w=req_w,
                main_h=req_h,
                preview_w=self._preview_size[0],
                preview_h=self._preview_size[1],
                fps=req_fps,
                zoom=self._zoom,
                ev=self._ev,
                transform=self._transform_str,
            )

        self._ensure_picamera2_imports()
        # Ruling 3: staging files end in .mp4.part so ffmpeg needs -f mp4 explicitly.
        output_target = _FfmpegOutputTarget("-f", "mp4", "-movflags", "+faststart", tmp_path)

        audio_recorded = False
        if audio:
            encoder = self._encoder_cls(bitrate=int(bitrate))
            output = self._ffmpeg_output_cls(
                output_target,
                audio=True,
                audio_device="default",
                audio_samplerate=48000,
                audio_codec="aac",
                audio_bitrate=128000,
            )
            try:
                self._picam2.start_recording(encoder, output)
                audio_recorded = True
            except Exception as exc:  # noqa: BLE001
                self._emit_event(
                    {
                        "event": "warn",
                        "code": "audio_unavailable",
                        "message": f"ALSA capture failed; recording video muted ({exc})",
                    }
                )
                encoder = self._encoder_cls(bitrate=int(bitrate))
                muted_output = self._ffmpeg_output_cls(output_target, audio=False)
                self._picam2.start_recording(encoder, muted_output)
                audio_recorded = False
        else:
            encoder = self._encoder_cls(bitrate=int(bitrate))
            muted_output = self._ffmpeg_output_cls(output_target, audio=False)
            self._picam2.start_recording(encoder, muted_output)
            audio_recorded = False

        self._recording = True
        self._video_tmp_path = tmp_path
        self._video_start_monotonic = time.monotonic()
        self._audio_recorded = audio_recorded
        return {"audio_recorded": audio_recorded}

    def stop_video(self) -> dict[str, Any]:
        if not self._recording:
            raise RuntimeError("not_recording")
        self._picam2.stop_recording()
        duration_sec = round(max(0.0, time.monotonic() - self._video_start_monotonic), 3)
        size_bytes = (
            os.path.getsize(self._video_tmp_path)
            if self._video_tmp_path and os.path.exists(self._video_tmp_path)
            else 0
        )
        audio_recorded = self._audio_recorded
        self._recording = False
        self._video_tmp_path = ""
        return {
            "duration_sec": duration_sec,
            "bytes": size_bytes,
            "audio_recorded": audio_recorded,
        }

    def pause(self) -> dict[str, Any]:
        if self._recording:
            self.stop_video()
        with self._state_lock:
            self._paused = True
            was_started = self._started
            self._started = False
            self._active_main_size = None
            self._active_config = None
            self._lores_stride = None
            self._main_stride = None
        if self._picam2 is not None:
            self._picam2.post_callback = None
            if was_started:
                self._picam2.stop()
            self._picam2.close()
            self._picam2 = None
        return {}

    def resume(self) -> dict[str, Any]:
        if not self._paused and self._picam2 is not None:
            return {}
        self._open_camera()
        with self._state_lock:
            self._paused = False
        if self._last_config_kwargs is not None:
            self.configure(**self._last_config_kwargs)
        return {}

    def shutdown(self) -> None:
        if self._recording:
            try:
                self.stop_video()
            except Exception:
                self._recording = False
        with self._state_lock:
            self._paused = True
            was_started = self._started
            self._started = False
            self._active_main_size = None
            self._active_config = None
        if self._picam2 is not None:
            try:
                self._picam2.post_callback = None
                if was_started:
                    self._picam2.stop()
                self._picam2.close()
            finally:
                self._picam2 = None
        self._frame_writer.stop()


class MockCameraDriver:
    """Synthetic animated camera driver for --mock-driver tests without hardware."""

    def __init__(
        self,
        frame_writer: FramePipeWriter,
        emit_event: Callable[[dict[str, Any]], None],
        has_audio: bool = True,
    ) -> None:
        self._frame_writer = frame_writer
        self._emit_event = emit_event
        self._has_audio = has_audio

        self._lock = threading.Lock()
        self._stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None

        self._configured = False
        self._paused = False
        self._recording = False
        self._mode = "photo"
        self._main_size = (3280, 2464)
        self._preview_size = (226, 170)
        self._fps = 30
        self._zoom = 1.0
        self._ev = 0.0
        self._transform = "auto"
        self._is_16_9 = False
        self._frame_counter = 0

        self._video_tmp_path = ""
        self._video_start_monotonic = 0.0
        self._audio_recorded = False

    def start(self) -> None:
        self._emit_event(
            {
                "event": "ready",
                "protocol": PROTOCOL_VERSION,
                "backend": "mock",
                "sensor": "mock-imx219",
                "has_audio": self._has_audio,
            }
        )
        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._preview_loop,
            name="mock-camera-preview",
            daemon=True,
        )
        self._thread.start()

    def _build_synthetic_yuv420(self, width: int, height: int, tick: int) -> bytes:
        y_size = width * height
        uv_size = (width // 2) * (height // 2)
        ev_shift = int(round(self._ev * 24.0))
        base_y = max(16, min(235, 110 + ev_shift))

        y_plane = bytearray(y_size)
        bar_x = (tick * 4) % max(1, width)
        zoom_band = max(4, int(round(16 * self._zoom)))
        for row in range(height):
            row_val = max(16, min(235, base_y + ((row * 40) // max(1, height)) - 20))
            offset = row * width
            y_plane[offset : offset + width] = bytes([row_val]) * width
            # Animated vertical marker moving horizontally
            for dx in range(min(zoom_band, width)):
                y_plane[offset + (bar_x + dx) % width] = 225

        u_plane = bytes([118]) * uv_size
        v_plane = bytes([142]) * uv_size
        return bytes(y_plane) + u_plane + v_plane

    def _preview_loop(self) -> None:
        while not self._stop_event.is_set():
            with self._lock:
                active = self._configured and not self._paused
                pw, ph = self._preview_size
                fps = max(1, min(60, self._fps))
                is_16_9 = self._is_16_9
                tick = self._frame_counter
                if active:
                    self._frame_counter += 1
                    payload = self._build_synthetic_yuv420(pw, ph, tick)
                else:
                    payload = b""
            if active:
                flags = FLAG_ASPECT_16_9 if is_16_9 else 0
                self._frame_writer.push_frame(pw, ph, PIXFMT_YUV420, flags, payload)
            self._stop_event.wait(1.0 / fps)

    def configure(
        self,
        mode: str,
        main_w: int,
        main_h: int,
        preview_w: int,
        preview_h: int,
        fps: int,
        zoom: float,
        ev: float,
        transform: str,
    ) -> dict[str, Any]:
        with self._lock:
            if self._recording:
                raise RuntimeError("camera_recording")
            pw = max(2, int(preview_w) & ~1)
            ph = max(2, int(preview_h) & ~1)
            mw = max(2, int(main_w) & ~1)
            mh = max(2, int(main_h) & ~1)
            self._mode = mode
            self._main_size = (mw, mh)
            self._preview_size = (pw, ph)
            self._fps = max(1, int(fps))
            self._zoom = max(1.0, min(4.0, float(zoom)))
            self._ev = max(-2.0, min(2.0, float(ev)))
            self._transform = transform
            self._is_16_9 = _is_16_9_mode(mode, mw, mh)
            self._configured = True
            self._paused = False
            return {
                "mode": self._mode,
                "main_w": mw,
                "main_h": mh,
                "preview_w": pw,
                "preview_h": ph,
            }

    def set_controls(
        self,
        zoom: Optional[float] = None,
        ev: Optional[float] = None,
    ) -> dict[str, Any]:
        with self._lock:
            if zoom is not None:
                self._zoom = max(1.0, min(4.0, float(zoom)))
            if ev is not None:
                self._ev = max(-2.0, min(2.0, float(ev)))
            return {"zoom": self._zoom, "ev": self._ev}

    def capture_still(self, tmp_path: str, width: int, height: int) -> dict[str, Any]:
        with self._lock:
            if not self._configured or self._paused:
                raise RuntimeError("camera_not_started")
            if self._recording:
                raise RuntimeError("camera_busy")
            ev = self._ev
        req_w = max(2, int(width) & ~1)
        req_h = max(2, int(height) & ~1)
        file_bytes = _write_minimal_jpeg(tmp_path, req_w, req_h, ev=ev)
        return {"width": req_w, "height": req_h, "bytes": file_bytes}

    def start_video(
        self,
        tmp_path: str,
        width: int,
        height: int,
        fps: int,
        bitrate: int,
        audio: bool,
    ) -> dict[str, Any]:
        del bitrate  # Unused in mock driver
        with self._lock:
            if self._paused:
                raise RuntimeError("camera_not_started")
            if self._recording:
                raise RuntimeError("already_recording")
            self._mode = "video"
            self._main_size = (max(2, int(width) & ~1), max(2, int(height) & ~1))
            self._fps = max(1, int(fps))
            self._is_16_9 = True
            self._configured = True
            self._recording = True
            self._video_tmp_path = tmp_path
            self._video_start_monotonic = time.monotonic()
            self._audio_recorded = bool(audio) and self._has_audio
            _write_minimal_mp4_stub(tmp_path)
            return {"audio_recorded": self._audio_recorded}

    def stop_video(self) -> dict[str, Any]:
        with self._lock:
            if not self._recording:
                raise RuntimeError("not_recording")
            duration_sec = round(max(0.001, time.monotonic() - self._video_start_monotonic), 3)
            size_bytes = (
                os.path.getsize(self._video_tmp_path)
                if self._video_tmp_path and os.path.exists(self._video_tmp_path)
                else 0
            )
            audio_recorded = self._audio_recorded
            self._recording = False
            self._video_tmp_path = ""
            return {
                "duration_sec": duration_sec,
                "bytes": size_bytes,
                "audio_recorded": audio_recorded,
            }

    def pause(self) -> dict[str, Any]:
        with self._lock:
            self._paused = True
            if self._recording:
                self._recording = False
                self._video_tmp_path = ""
        return {}

    def resume(self) -> dict[str, Any]:
        with self._lock:
            self._paused = False
        return {}

    def shutdown(self) -> None:
        self._stop_event.set()
        thread = self._thread
        if thread is not None and thread.is_alive():
            thread.join(timeout=1.0)
        self._thread = None
        self._frame_writer.stop()
