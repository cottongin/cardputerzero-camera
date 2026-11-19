import io
import json
import os
import select
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

from czcamera_cam.protocol import (
    CZFR_HEADER_STRUCT,
    CZFR_MAGIC,
    FLAG_ASPECT_16_9,
    PIXFMT_RGB24,
    PIXFMT_YUV420,
    FramePipeWriter,
    JsonProtocolServer,
    pack_czfr_header,
)


def _read_exact(fd: int, n: int, timeout: float = 5.0) -> bytes:
    buf = bytearray()
    deadline = time.monotonic() + timeout
    while len(buf) < n:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"Timed out reading {n} bytes from fd {fd} (got {len(buf)})")
        rlist, _, _ = select.select([fd], [], [], remaining)
        if not rlist:
            continue
        chunk = os.read(fd, n - len(buf))
        if not chunk:
            raise EOFError(f"EOF on fd {fd} after {len(buf)}/{n} bytes")
        buf.extend(chunk)
    return bytes(buf)


def _read_czfr_frame(fd: int, timeout: float = 5.0):
    hdr = _read_exact(fd, CZFR_HEADER_STRUCT.size, timeout=timeout)
    magic, width, height, pixfmt, flags, reserved, payload_len = CZFR_HEADER_STRUCT.unpack(hdr)
    payload = _read_exact(fd, payload_len, timeout=timeout)
    return magic, width, height, pixfmt, flags, reserved, payload


class ProtocolTest(unittest.TestCase):
    def test_czfr_frame_header_packing(self):
        payload_len = 226 * 170 * 3 // 2
        hdr = pack_czfr_header(226, 170, PIXFMT_YUV420, 0, payload_len)
        self.assertEqual(len(hdr), 16)
        self.assertEqual(
            hdr,
            struct.pack("<4sHHBBHI", b"CZFR", 226, 170, 1, 0, 0, payload_len),
        )
        magic, w, h, pixfmt, flags, reserved, plen = struct.unpack("<4sHHBBHI", hdr)
        self.assertEqual(magic, CZFR_MAGIC)
        self.assertEqual(w, 226)
        self.assertEqual(h, 170)
        self.assertEqual(pixfmt, PIXFMT_YUV420)
        self.assertEqual(flags, 0)
        self.assertEqual(reserved, 0)
        self.assertEqual(plen, payload_len)

        hdr_16_9 = pack_czfr_header(226, 128, PIXFMT_RGB24, FLAG_ASPECT_16_9, 226 * 128 * 3)
        self.assertEqual(struct.unpack("<4sHHBBHI", hdr_16_9)[3:5], (2, 1))

    def test_frame_pipe_writer_drops_stale_frames_under_backpressure(self):
        r_fd, w_fd = os.pipe()
        writer = FramePipeWriter(w_fd, owns_fd=False)
        writer.start()
        try:
            # Fill the pipe buffer first by writing directly to w_fd or pushing
            # large frames without reading from r_fd.
            # Each frame is 128 KiB, so after 1-2 frames the OS pipe buffer fills.
            frame_size = 128 * 1024

            # Push frame 0 to get the writer thread blocked in os.write / select.
            writer.push_frame(226, 170, PIXFMT_YUV420, 0, b"\x00" * frame_size)
            time.sleep(0.05)

            # Now push 20 frames in a tight loop while consumer is stalled.
            t0 = time.monotonic()
            for idx in range(1, 21):
                payload = bytes([idx]) * frame_size
                writer.push_frame(226, 170, PIXFMT_YUV420, 0, payload)
            elapsed = time.monotonic() - t0

            # push_frame() must never block on the stalled pipe.
            self.assertLess(elapsed, 0.2, f"push_frame stalled for {elapsed:.3f}s")
            self.assertGreater(writer.frames_dropped, 0)

            # Drain all frames available until we see the latest (idx == 20).
            seen_markers = []
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline:
                magic, w, h, pixfmt, flags, reserved, payload = _read_czfr_frame(r_fd, timeout=2.0)
                self.assertEqual(magic, b"CZFR")
                self.assertEqual((w, h, pixfmt, flags, reserved), (226, 170, PIXFMT_YUV420, 0, 0))
                self.assertEqual(len(payload), frame_size)
                seen_markers.append(payload[0])
                if payload[0] == 20:
                    break

            self.assertIn(20, seen_markers)
            self.assertLess(len(seen_markers), 21)
        finally:
            writer.stop()
            os.close(r_fd)
            os.close(w_fd)

    def test_cli_mock_driver_end_to_end(self):
        repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        bin_path = os.path.join(repo_root, "cam-helper", "bin", "czcamera-cam")
        self.assertTrue(os.path.isfile(bin_path), f"Missing executable {bin_path}")
        self.assertTrue(os.access(bin_path, os.X_OK), f"{bin_path} must be executable")

        r_fd, w_fd = os.pipe()
        os.set_inheritable(w_fd, True)

        with tempfile.TemporaryDirectory() as tmpdir:
            still_tmp = os.path.join(tmpdir, ".IMG_test.jpg.tmp")
            video_tmp = os.path.join(tmpdir, ".VID_test.mp4.part")

            proc = subprocess.Popen(
                [bin_path, "--frame-fd", str(w_fd), "--mock-driver"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                pass_fds=(w_fd,),
                text=True,
                bufsize=1,
            )
            os.close(w_fd)

            def send_cmd(obj: dict) -> dict:
                assert proc.stdin is not None and proc.stdout is not None
                proc.stdin.write(json.dumps(obj) + "\n")
                proc.stdin.flush()
                while True:
                    line = proc.stdout.readline()
                    self.assertTrue(line, "Unexpected EOF from czcamera-cam stdout")
                    msg = json.loads(line)
                    if "id" in msg and msg["id"] == obj["id"]:
                        return msg

            try:
                assert proc.stdout is not None
                ready_line = proc.stdout.readline()
                self.assertTrue(ready_line, "Expected ready event on startup")
                ready = json.loads(ready_line)
                self.assertEqual(ready["event"], "ready")
                self.assertEqual(ready["protocol"], 1)
                self.assertEqual(ready["backend"], "mock")
                self.assertIn("has_audio", ready)

                # Configure 4:3 photo mode
                rep = send_cmd(
                    {
                        "id": 1,
                        "cmd": "configure",
                        "mode": "photo",
                        "main_w": 3280,
                        "main_h": 2464,
                        "preview_w": 226,
                        "preview_h": 170,
                        "fps": 30,
                        "zoom": 1.0,
                        "ev": 0.0,
                        "transform": "auto",
                    }
                )
                self.assertTrue(rep["ok"])

                # Expect at least one CZFR frame on frame_fd
                magic, w, h, pixfmt, flags, reserved, payload = _read_czfr_frame(r_fd, timeout=3.0)
                self.assertEqual(magic, b"CZFR")
                self.assertEqual((w, h), (226, 170))
                self.assertEqual(pixfmt, PIXFMT_YUV420)
                self.assertEqual(flags, 0)
                self.assertEqual(reserved, 0)
                self.assertEqual(len(payload), 226 * 170 * 3 // 2)

                # Live set_controls
                rep = send_cmd({"id": 2, "cmd": "set_controls", "zoom": 2.0, "ev": -0.5})
                self.assertTrue(rep["ok"])

                # Still capture writes valid JPEG to tmp_path
                rep = send_cmd(
                    {
                        "id": 3,
                        "cmd": "capture_still",
                        "tmp_path": still_tmp,
                        "width": 640,
                        "height": 480,
                    }
                )
                self.assertTrue(rep["ok"])
                self.assertEqual(rep["width"], 640)
                self.assertEqual(rep["height"], 480)
                self.assertGreater(rep["bytes"], 100)
                self.assertTrue(os.path.isfile(still_tmp))
                with open(still_tmp, "rb") as f:
                    jpg = f.read()
                self.assertTrue(jpg.startswith(b"\xff\xd8"))
                self.assertTrue(jpg.endswith(b"\xff\xd9"))

                # Video configure + start_video -> stop_video
                rep = send_cmd(
                    {
                        "id": 4,
                        "cmd": "configure",
                        "mode": "video",
                        "main_w": 1280,
                        "main_h": 720,
                        "preview_w": 226,
                        "preview_h": 128,
                        "fps": 30,
                        "zoom": 1.0,
                        "ev": 0.0,
                        "transform": "0",
                    }
                )
                self.assertTrue(rep["ok"])

                rep = send_cmd(
                    {
                        "id": 5,
                        "cmd": "start_video",
                        "tmp_path": video_tmp,
                        "width": 1280,
                        "height": 720,
                        "fps": 30,
                        "bitrate": 4000000,
                        "audio": True,
                    }
                )
                self.assertTrue(rep["ok"])
                self.assertIn("audio_recorded", rep)

                # Drain until we see a 16:9 frame while recording
                saw_16_9 = False
                for _ in range(30):
                    magic, w, h, pixfmt, flags, _, _ = _read_czfr_frame(r_fd, timeout=3.0)
                    if (w, h) == (226, 128) and (flags & FLAG_ASPECT_16_9):
                        saw_16_9 = True
                        break
                self.assertTrue(saw_16_9, "Expected 16:9 CZFR preview frame during video recording")

                time.sleep(0.05)
                rep = send_cmd({"id": 6, "cmd": "stop_video"})
                self.assertTrue(rep["ok"])
                self.assertGreater(rep["duration_sec"], 0.0)
                self.assertGreater(rep["bytes"], 0)
                self.assertIn("audio_recorded", rep)
                self.assertTrue(os.path.isfile(video_tmp))

                # Pause & resume
                rep = send_cmd({"id": 7, "cmd": "pause"})
                self.assertTrue(rep["ok"])
                rep = send_cmd({"id": 8, "cmd": "resume"})
                self.assertTrue(rep["ok"])

                # Shutdown
                rep = send_cmd({"id": 9, "cmd": "shutdown"})
                self.assertTrue(rep["ok"])

                rc = proc.wait(timeout=5.0)
                self.assertEqual(rc, 0, f"stderr={proc.stderr.read() if proc.stderr else ''}")
            finally:
                os.close(r_fd)
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5.0)
                if proc.stdin:
                    proc.stdin.close()
                if proc.stdout:
                    proc.stdout.close()
                if proc.stderr:
                    proc.stderr.close()

    def test_invalid_json_and_unknown_command_do_not_crash(self):
        repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        bin_path = os.path.join(repo_root, "cam-helper", "bin", "czcamera-cam")
        r_fd, w_fd = os.pipe()
        os.set_inheritable(w_fd, True)

        proc = subprocess.Popen(
            [bin_path, "--frame-fd", str(w_fd), "--mock-driver"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            pass_fds=(w_fd,),
            text=True,
            bufsize=1,
        )
        os.close(w_fd)
        try:
            assert proc.stdin is not None and proc.stdout is not None
            ready = json.loads(proc.stdout.readline())
            self.assertEqual(ready["event"], "ready")

            # Malformed JSON triggers warn without exiting
            proc.stdin.write("{not-valid-json\n")
            proc.stdin.flush()
            warn_msg = json.loads(proc.stdout.readline())
            self.assertEqual(warn_msg["event"], "warn")
            self.assertEqual(warn_msg["code"], "bad_command")

            # Unknown command returns ok=False
            proc.stdin.write(json.dumps({"id": 42, "cmd": "bogus"}) + "\n")
            proc.stdin.flush()
            err_msg = json.loads(proc.stdout.readline())
            self.assertEqual(err_msg["id"], 42)
            self.assertFalse(err_msg["ok"])
            self.assertIn("unknown_cmd", err_msg["error"])

            # Clean shutdown still works
            proc.stdin.write(json.dumps({"id": 43, "cmd": "shutdown"}) + "\n")
            proc.stdin.flush()
            bye = json.loads(proc.stdout.readline())
            self.assertEqual(bye, {"id": 43, "ok": True})
            self.assertEqual(proc.wait(timeout=5.0), 0)
        finally:
            os.close(r_fd)
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5.0)
            if proc.stdin:
                proc.stdin.close()
            if proc.stdout:
                proc.stdout.close()
            if proc.stderr:
                proc.stderr.close()

    def test_sigterm_exits_cleanly_without_reentrant_error(self):
        repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        bin_path = os.path.join(repo_root, "cam-helper", "bin", "czcamera-cam")
        r_fd, w_fd = os.pipe()
        os.set_inheritable(w_fd, True)

        proc = subprocess.Popen(
            [bin_path, "--frame-fd", str(w_fd), "--mock-driver"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            pass_fds=(w_fd,),
            text=True,
            bufsize=1,
        )
        os.close(w_fd)
        try:
            assert proc.stdin is not None and proc.stdout is not None and proc.stderr is not None
            ready = json.loads(proc.stdout.readline())
            self.assertEqual(ready["event"], "ready")

            proc.stdin.write(
                json.dumps(
                    {
                        "id": 1,
                        "cmd": "configure",
                        "mode": "photo",
                        "main_w": 3280,
                        "main_h": 2464,
                        "preview_w": 226,
                        "preview_h": 170,
                        "fps": 30,
                        "zoom": 1.0,
                        "ev": 0.0,
                        "transform": "auto",
                    }
                )
                + "\n"
            )
            proc.stdin.flush()
            rep = json.loads(proc.stdout.readline())
            self.assertTrue(rep["ok"])

            # Leave stdin open while blocked waiting for commands, then send SIGTERM.
            proc.terminate()
            rc = proc.wait(timeout=5.0)
            stderr_out = proc.stderr.read()
            self.assertEqual(rc, 0, f"Unexpected non-zero exit code {rc}, stderr={stderr_out!r}")
            self.assertEqual(stderr_out, "")
        finally:
            os.close(r_fd)
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=5.0)
            if proc.stdin:
                proc.stdin.close()
            if proc.stdout:
                proc.stdout.close()
            if proc.stderr:
                proc.stderr.close()

    def test_frame_pipe_writer_validates_header_in_push_frame(self):
        r_fd, w_fd = os.pipe()
        thread_errors = []
        orig_hook = threading.excepthook
        threading.excepthook = lambda args: thread_errors.append(args)
        writer = FramePipeWriter(w_fd, owns_fd=False)
        writer.start()
        try:
            with self.assertRaises(ValueError):
                writer.push_frame(226, 170, 99, 0, b"\x00")
            with self.assertRaises(ValueError):
                writer.push_frame(70000, 170, PIXFMT_YUV420, 0, b"\x00")
            self.assertEqual(thread_errors, [])
        finally:
            writer.stop()
            threading.excepthook = orig_hook
            os.close(r_fd)
            os.close(w_fd)

    def test_frame_pipe_writer_select_errors_and_safe_close(self):
        # 1. InterruptedError retries select, then OSError / ValueError exit cleanly without thread crash
        for terminal_exc in (OSError("EBADF"), ValueError("fd out of range")):
            r_fd, w_fd = os.pipe()
            thread_errors = []
            orig_hook = threading.excepthook
            threading.excepthook = lambda args: thread_errors.append(args)
            writer = FramePipeWriter(w_fd, owns_fd=False)
            select_calls = []

            def fake_select(_r, wlist, _x, _timeout):
                select_calls.append(len(select_calls))
                if len(select_calls) == 1:
                    raise InterruptedError("EINTR")
                raise terminal_exc

            try:
                with mock.patch("czcamera_cam.protocol.os.write", side_effect=BlockingIOError), \
                     mock.patch("czcamera_cam.protocol.select.select", side_effect=fake_select):
                    writer.start()
                    writer.push_frame(4, 4, PIXFMT_YUV420, 0, b"\x00" * 24)
                    deadline = time.monotonic() + 2.0
                    while len(select_calls) < 2 and time.monotonic() < deadline:
                        time.sleep(0.01)
                    time.sleep(0.05)
                self.assertEqual(len(select_calls), 2)
                self.assertEqual(thread_errors, [])
            finally:
                writer.stop()
                threading.excepthook = orig_hook
                os.close(r_fd)
                os.close(w_fd)

        # 2. owns_fd=True must NOT close fd while writer thread is still alive on stop() timeout
        r_fd, w_fd = os.pipe()
        writer = FramePipeWriter(w_fd, owns_fd=True)
        write_started = threading.Event()
        release_write = threading.Event()
        closed_fds = []
        real_close = os.close

        def slow_write(_fd, buf):
            write_started.set()
            release_write.wait(timeout=2.0)
            return len(buf)

        def tracked_close(fd):
            closed_fds.append(fd)
            return real_close(fd)

        try:
            with mock.patch("czcamera_cam.protocol.os.write", side_effect=slow_write), \
                 mock.patch("czcamera_cam.protocol.os.close", side_effect=tracked_close):
                writer.start()
                writer.push_frame(4, 4, PIXFMT_YUV420, 0, b"\x00" * 24)
                self.assertTrue(write_started.wait(timeout=2.0))

                # Timeout expires while slow_write is still blocked; fd must NOT be closed yet.
                writer.stop(timeout=0.02)
                self.assertNotIn(w_fd, closed_fds)

                # Unblock writer thread; thread finishes and closes w_fd exactly once.
                release_write.set()
                deadline = time.monotonic() + 2.0
                while w_fd not in closed_fds and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertEqual(closed_fds.count(w_fd), 1)
                writer.stop(timeout=0.1)
                self.assertEqual(closed_fds.count(w_fd), 1)
        finally:
            release_write.set()
            real_close(r_fd)
            if w_fd not in closed_fds:
                real_close(w_fd)

    def test_json_protocol_server_calls_shutdown_if_worker_start_raises(self):
        events = []

        class FailingStartWorker:
            def start(self):
                events.append("start")
                raise RuntimeError("camera open failed")

            def shutdown(self):
                events.append("shutdown")

        server = JsonProtocolServer(
            worker_factory=lambda _emit: FailingStartWorker(),
            stdin=io.StringIO(""),
            stdout=io.StringIO(""),
        )
        with self.assertRaisesRegex(RuntimeError, "camera open failed"):
            server.run()
        self.assertEqual(events, ["start", "shutdown"])


if __name__ == "__main__":
    unittest.main()
