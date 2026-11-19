"""Wire Protocol v1: CZFR binary frame pipe writer and newline-JSON control server."""

from __future__ import annotations

import json
import os
import select
import struct
import threading
from typing import Any, Callable, Optional, TextIO

PROTOCOL_VERSION = 1
CZFR_MAGIC = b"CZFR"
CZFR_HEADER_STRUCT = struct.Struct("<4sHHBBHI")

PIXFMT_YUV420 = 1
PIXFMT_RGB24 = 2

FLAG_ASPECT_16_9 = 0x01


def pack_czfr_header(
    width: int,
    height: int,
    pixfmt: int,
    flags: int,
    payload_bytes: int,
) -> bytes:
    """Pack the 16-byte little-endian CZFR frame header."""
    if not (0 <= width <= 0xFFFF and 0 <= height <= 0xFFFF):
        raise ValueError(f"Frame dimensions out of uint16 range: {width}x{height}")
    if pixfmt not in (PIXFMT_YUV420, PIXFMT_RGB24):
        raise ValueError(f"Unsupported pixfmt: {pixfmt}")
    if not (0 <= flags <= 0xFF):
        raise ValueError(f"Flags out of uint8 range: {flags}")
    if not (0 <= payload_bytes <= 0xFFFFFFFF):
        raise ValueError(f"Payload length out of uint32 range: {payload_bytes}")
    return CZFR_HEADER_STRUCT.pack(
        CZFR_MAGIC,
        width,
        height,
        pixfmt,
        flags,
        0,
        payload_bytes,
    )


class FramePipeWriter:
    """Single-slot latest-frame writer for the dedicated CZFR frame pipe.

    push_frame() never blocks on pipe backpressure: if a previous unconsumed frame
    is still queued, it is discarded and replaced by the newest frame.
    """

    def __init__(self, frame_fd: int, owns_fd: bool = False) -> None:
        if frame_fd < 0:
            raise ValueError(f"Invalid frame_fd: {frame_fd}")
        self._fd = frame_fd
        self._owns_fd = owns_fd
        self._cond = threading.Condition()
        self._pending: Optional[tuple[int, int, int, int, bytes]] = None
        self._stop = False
        self._started = False
        self._thread: Optional[threading.Thread] = None
        self._frames_pushed = 0
        self._frames_dropped = 0
        self._frames_written = 0

        try:
            os.set_blocking(self._fd, False)
        except OSError:
            pass

    @property
    def frames_pushed(self) -> int:
        with self._cond:
            return self._frames_pushed

    @property
    def frames_dropped(self) -> int:
        with self._cond:
            return self._frames_dropped

    @property
    def frames_written(self) -> int:
        with self._cond:
            return self._frames_written

    def start(self) -> None:
        with self._cond:
            if self._started:
                return
            self._stop = False
            self._started = True
            self._thread = threading.Thread(
                target=self._writer_loop,
                name="czfr-pipe-writer",
                daemon=True,
            )
            self._thread.start()

    def push_frame(
        self,
        width: int,
        height: int,
        pixfmt: int,
        flags: int,
        payload: bytes | bytearray | memoryview,
    ) -> None:
        """Queue the latest frame without blocking; drop any stale queued frame."""
        data = bytes(payload)
        with self._cond:
            if self._stop:
                return
            if self._pending is not None:
                self._frames_dropped += 1
            self._pending = (width, height, pixfmt, flags, data)
            self._frames_pushed += 1
            self._cond.notify()

    def stop(self, timeout: float = 1.0) -> None:
        with self._cond:
            self._stop = True
            self._pending = None
            self._cond.notify_all()
            thread = self._thread
        if thread is not None and thread.is_alive():
            thread.join(timeout=timeout)
        with self._cond:
            self._thread = None
            self._started = False
        if self._owns_fd and self._fd >= 0:
            try:
                os.close(self._fd)
            except OSError:
                pass
            self._fd = -1

    def _write_packet(self, packet: bytes) -> bool:
        mv = memoryview(packet)
        offset = 0
        total = len(packet)
        try:
            while offset < total:
                with self._cond:
                    if self._stop:
                        return False
                try:
                    written = os.write(self._fd, mv[offset:])
                except BlockingIOError:
                    select.select([], [self._fd], [], 0.05)
                    continue
                except (BrokenPipeError, OSError):
                    return False
                if written <= 0:
                    return False
                offset += written
            return True
        finally:
            mv.release()

    def _writer_loop(self) -> None:
        while True:
            with self._cond:
                while self._pending is None and not self._stop:
                    self._cond.wait()
                if self._stop:
                    return
                frame = self._pending
                self._pending = None

            assert frame is not None
            width, height, pixfmt, flags, payload = frame
            header = pack_czfr_header(width, height, pixfmt, flags, len(payload))
            if not self._write_packet(header + payload):
                with self._cond:
                    self._stop = True
                return
            with self._cond:
                self._frames_written += 1


class JsonProtocolServer:
    """Newline-delimited JSON control channel on stdin/stdout."""

    def __init__(
        self,
        worker_factory: Callable[[Callable[[dict[str, Any]], None]], Any],
        stdin: TextIO,
        stdout: TextIO,
        wakeup_fd: Optional[int] = None,
    ) -> None:
        self._stdin = stdin
        self._stdout = stdout
        self._wakeup_fd = wakeup_fd
        self._write_lock = threading.Lock()
        self._shutdown_requested = False
        self._worker = worker_factory(self.emit_event)

    def emit_event(self, payload: dict[str, Any]) -> None:
        self._write_json(payload)

    def request_shutdown(self) -> None:
        self._shutdown_requested = True

    def _write_json(self, payload: dict[str, Any]) -> None:
        line = json.dumps(payload, separators=(",", ":")) + "\n"
        with self._write_lock:
            try:
                self._stdout.write(line)
                self._stdout.flush()
            except (BrokenPipeError, OSError, ValueError):
                self._shutdown_requested = True

    def _reply_ok(self, cmd_id: Any, extra: Optional[dict[str, Any]] = None) -> None:
        reply: dict[str, Any] = {"id": cmd_id, "ok": True}
        if extra:
            reply.update(extra)
        self._write_json(reply)

    def _reply_err(self, cmd_id: Any, error: str) -> None:
        self._write_json({"id": cmd_id, "ok": False, "error": error})

    def _handle_command(self, msg: dict[str, Any]) -> None:
        cmd_id = msg.get("id")
        cmd = msg.get("cmd")
        if not isinstance(cmd, str):
            self._reply_err(cmd_id, "missing_cmd")
            return

        try:
            if cmd == "configure":
                res = self._worker.configure(
                    mode=str(msg.get("mode", "photo")),
                    main_w=int(msg["main_w"]),
                    main_h=int(msg["main_h"]),
                    preview_w=int(msg.get("preview_w", 226)),
                    preview_h=int(msg.get("preview_h", 170)),
                    fps=int(msg.get("fps", 30)),
                    zoom=float(msg.get("zoom", 1.0)),
                    ev=float(msg.get("ev", 0.0)),
                    transform=str(msg.get("transform", "auto")),
                )
                self._reply_ok(cmd_id, res)
            elif cmd == "set_controls":
                res = self._worker.set_controls(
                    zoom=float(msg["zoom"]) if "zoom" in msg else None,
                    ev=float(msg["ev"]) if "ev" in msg else None,
                )
                self._reply_ok(cmd_id, res)
            elif cmd == "capture_still":
                res = self._worker.capture_still(
                    tmp_path=str(msg["tmp_path"]),
                    width=int(msg["width"]),
                    height=int(msg["height"]),
                )
                self._reply_ok(cmd_id, res)
            elif cmd == "start_video":
                res = self._worker.start_video(
                    tmp_path=str(msg["tmp_path"]),
                    width=int(msg["width"]),
                    height=int(msg["height"]),
                    fps=int(msg.get("fps", 30)),
                    bitrate=int(msg.get("bitrate", 4000000)),
                    audio=bool(msg.get("audio", True)),
                )
                self._reply_ok(cmd_id, res)
            elif cmd == "stop_video":
                res = self._worker.stop_video()
                self._reply_ok(cmd_id, res)
            elif cmd == "pause":
                res = self._worker.pause()
                self._reply_ok(cmd_id, res)
            elif cmd == "resume":
                res = self._worker.resume()
                self._reply_ok(cmd_id, res)
            elif cmd == "shutdown":
                self._worker.shutdown()
                self._reply_ok(cmd_id)
                self._shutdown_requested = True
            else:
                self._reply_err(cmd_id, f"unknown_cmd:{cmd}")
        except Exception as exc:  # noqa: BLE001
            self._reply_err(cmd_id, str(exc))

    def _get_stdin_fd(self) -> Optional[int]:
        try:
            fd = self._stdin.fileno()
        except (AttributeError, OSError, ValueError):
            return None
        return fd if isinstance(fd, int) and fd >= 0 else None

    def _read_line_fd(self, stdin_fd: int, wakeup_fd: int, buf: bytearray) -> str:
        while True:
            nl = buf.find(b"\n")
            if nl != -1:
                line_bytes = bytes(buf[: nl + 1])
                del buf[: nl + 1]
                return line_bytes.decode("utf-8", errors="replace")

            if self._shutdown_requested:
                return ""

            try:
                rlist, _, _ = select.select([stdin_fd, wakeup_fd], [], [], 0.25)
            except (InterruptedError, OSError, ValueError):
                if self._shutdown_requested:
                    return ""
                continue

            if wakeup_fd in rlist:
                while True:
                    try:
                        drained = os.read(wakeup_fd, 256)
                        if not drained:
                            break
                    except BlockingIOError:
                        break
                    except OSError:
                        break
                if self._shutdown_requested:
                    return ""

            if stdin_fd in rlist:
                try:
                    chunk = os.read(stdin_fd, 4096)
                except BlockingIOError:
                    continue
                except OSError:
                    return ""
                if not chunk:
                    if buf:
                        tail = bytes(buf).decode("utf-8", errors="replace")
                        buf.clear()
                        return tail
                    return ""
                buf.extend(chunk)

    def run(self) -> int:
        self._worker.start()
        stdin_fd = self._get_stdin_fd() if self._wakeup_fd is not None else None
        fd_buf = bytearray()
        try:
            while not self._shutdown_requested:
                if stdin_fd is not None and self._wakeup_fd is not None:
                    raw_line = self._read_line_fd(stdin_fd, self._wakeup_fd, fd_buf)
                else:
                    try:
                        raw_line = self._stdin.readline()
                    except (OSError, ValueError):
                        break
                if not raw_line:
                    break
                line = raw_line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError as exc:
                    self.emit_event(
                        {
                            "event": "warn",
                            "code": "bad_command",
                            "message": f"Invalid JSON command: {exc}",
                        }
                    )
                    continue
                if not isinstance(msg, dict):
                    self.emit_event(
                        {
                            "event": "warn",
                            "code": "bad_command",
                            "message": "Command must be a JSON object",
                        }
                    )
                    continue
                self._handle_command(msg)
        finally:
            self._worker.shutdown()
        return 0
