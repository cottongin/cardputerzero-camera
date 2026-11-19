import os
import tempfile
import unittest

from czcamera_cam.picam2_worker import (
    Picam2Worker,
    compute_scaler_crop,
    resolve_transform,
)
from czcamera_cam.protocol import FLAG_ASPECT_16_9, PIXFMT_YUV420


class FakeTransform:
    def __init__(self, hflip=0, vflip=0):
        self.hflip = int(hflip)
        self.vflip = int(vflip)

    def __eq__(self, other):
        return (
            isinstance(other, FakeTransform)
            and self.hflip == other.hflip
            and self.vflip == other.vflip
        )

    def __repr__(self):
        return f"FakeTransform(hflip={self.hflip}, vflip={self.vflip})"


class FakeFrameWriter:
    def __init__(self):
        self.frames = []

    def push_frame(self, width, height, pixfmt, flags, payload):
        self.frames.append((width, height, pixfmt, flags, bytes(payload)))


class FakeRequest:
    def __init__(self, width, height, stride=None):
        self.width = width
        self.height = height
        self.stride = stride or width

    def make_buffer(self, name):
        assert name == "lores"
        # Produce Y plane (height rows of stride) + U (height//2 rows of stride//2) + V
        y_rows = b"".join(bytes([100] * self.width + [255] * (self.stride - self.width)) for _ in range(self.height))
        u_stride = self.stride // 2
        u_w = self.width // 2
        u_rows = b"".join(bytes([128] * u_w + [255] * (u_stride - u_w)) for _ in range(self.height // 2))
        v_rows = b"".join(bytes([129] * u_w + [255] * (u_stride - u_w)) for _ in range(self.height // 2))
        return y_rows + u_rows + v_rows


class FakeH264Encoder:
    def __init__(self, bitrate=4000000):
        self.bitrate = bitrate


class FakeFfmpegOutput:
    def __init__(
        self,
        output_filename,
        audio=False,
        audio_device="default",
        audio_samplerate=48000,
        audio_codec="aac",
        audio_bitrate=128000,
    ):
        self.output_filename = output_filename
        self.audio = audio
        self.audio_device = audio_device
        self.audio_samplerate = audio_samplerate
        self.audio_codec = audio_codec
        self.audio_bitrate = audio_bitrate


class FakePicamera2:
    instances = []
    fail_audio_on_start = False

    def __init__(self, rotation=180):
        self.camera_properties = {
            "PixelArraySize": (3280, 2464),
            "Model": "imx219",
            "Rotation": rotation,
        }
        self.configured = None
        self.started = False
        self.closed = False
        self.controls_history = []
        self.post_callback = None
        self.pre_callback = None
        self.recording = False
        self.recording_attempts = []
        self.captured_files = []
        self.encoder = None
        self.output = None
        FakePicamera2.instances.append(self)

    def create_still_configuration(self, main=None, lores=None, transform=None, **kwargs):
        return {
            "type": "still",
            "main": main,
            "lores": lores,
            "transform": transform,
            **kwargs,
        }

    def create_video_configuration(self, main=None, lores=None, transform=None, controls=None, **kwargs):
        return {
            "type": "video",
            "main": main,
            "lores": lores,
            "transform": transform,
            "controls": controls,
            **kwargs,
        }

    def configure(self, config):
        self.configured = config

    def start(self):
        self.started = True

    def stop(self):
        self.started = False

    def close(self):
        self.closed = True

    def set_controls(self, controls):
        self.controls_history.append(dict(controls))

    def capture_file(self, file_output, name="main", format=None):
        self.captured_files.append((file_output, name, format))
        with open(file_output, "wb") as f:
            f.write(b"\xff\xd8\xff\xe0MOCKJPEG\xff\xd9")

    def switch_mode_and_capture_file(self, camera_config, file_output, name="main", format=None):
        self.configured = camera_config
        self.capture_file(file_output, name=name, format=format)

    def start_recording(self, encoder, output):
        self.recording_attempts.append((encoder, output))
        if FakePicamera2.fail_audio_on_start and getattr(output, "audio", False):
            raise RuntimeError("ALSA device hw:0,0 unavailable")
        self.encoder = encoder
        self.output = output
        self.recording = True
        # Extract real path from output_filename string (after -f mp4 -movflags +faststart )
        path = output.output_filename.split()[-1].strip("'\"")
        with open(path, "wb") as f:
            f.write(b"\x00\x00\x00\x18ftypmp42MOCKMP4")

    def stop_recording(self):
        self.recording = False

    def emit_lores_frame(self, stride=None):
        lores = self.configured["lores"]
        w, h = lores["size"]
        req = FakeRequest(w, h, stride=stride)
        cb = self.post_callback or self.pre_callback
        if cb is not None:
            cb(req)


class Picam2WorkerTest(unittest.TestCase):
    def setUp(self):
        FakePicamera2.instances.clear()
        FakePicamera2.fail_audio_on_start = False

    def _make_worker(self, rotation=180):
        writer = FakeFrameWriter()
        events = []

        def picam_factory():
            return FakePicamera2(rotation=rotation)

        worker = Picam2Worker(
            frame_writer=writer,
            emit_event=events.append,
            picamera2_cls=picam_factory,
            transform_cls=FakeTransform,
            encoder_cls=FakeH264Encoder,
            ffmpeg_output_cls=FakeFfmpegOutput,
            audio_probe=lambda: True,
        )
        return worker, writer, events

    def test_scaler_crop_4_3_and_16_9_zoom_bounds(self):
        sensor = (3280, 2464)

        # 4:3 zoom=1.0 -> full sensor
        self.assertEqual(compute_scaler_crop(sensor, 4, 3, 1.0), (0, 0, 3280, 2464))

        # 16:9 zoom=1.0 -> centered 16:9 within full 3280 width
        self.assertEqual(compute_scaler_crop(sensor, 16, 9, 1.0), (0, 310, 3280, 1844))

        # zoom=2.0 halves dimensions centered
        self.assertEqual(compute_scaler_crop(sensor, 4, 3, 2.0), (820, 616, 1640, 1232))
        x16, y16, w16, h16 = compute_scaler_crop(sensor, 16, 9, 2.0)
        self.assertEqual((w16, h16), (1640, 922))
        self.assertEqual(x16, 820)
        self.assertEqual(y16 % 2, 0)
        self.assertGreaterEqual(y16, 310)

        # zoom=4.0 -> even-aligned non-negative bounds within sensor
        for aspect in ((4, 3), (16, 9)):
            x, y, w, h = compute_scaler_crop(sensor, aspect[0], aspect[1], 4.0)
            self.assertGreaterEqual(x, 0)
            self.assertGreaterEqual(y, 0)
            self.assertGreater(w, 0)
            self.assertGreater(h, 0)
            self.assertLessEqual(x + w, sensor[0])
            self.assertLessEqual(y + h, sensor[1])
            self.assertEqual(x % 2, 0)
            self.assertEqual(y % 2, 0)
            self.assertEqual(w % 2, 0)
            self.assertEqual(h % 2, 0)

    def test_resolve_transform_auto_and_explicit(self):
        self.assertEqual(
            resolve_transform("auto", {"Rotation": 180}, transform_cls=FakeTransform),
            FakeTransform(hflip=1, vflip=1),
        )
        self.assertEqual(
            resolve_transform("auto", {"Rotation": 0}, transform_cls=FakeTransform),
            FakeTransform(hflip=0, vflip=0),
        )
        self.assertEqual(
            resolve_transform("180", {"Rotation": 0}, transform_cls=FakeTransform),
            FakeTransform(hflip=1, vflip=1),
        )
        self.assertEqual(
            resolve_transform("0", {"Rotation": 180}, transform_cls=FakeTransform),
            FakeTransform(hflip=0, vflip=0),
        )
        self.assertEqual(
            resolve_transform("hflip", {"Rotation": 0}, transform_cls=FakeTransform),
            FakeTransform(hflip=1, vflip=0),
        )
        self.assertEqual(
            resolve_transform("vflip", {"Rotation": 0}, transform_cls=FakeTransform),
            FakeTransform(hflip=0, vflip=1),
        )

    def test_dual_stream_preview_continues_during_video_recording(self):
        worker, writer, events = self._make_worker()
        worker.start()
        self.assertEqual(events[0]["event"], "ready")
        self.assertEqual(events[0]["protocol"], 1)
        self.assertEqual(events[0]["backend"], "picamera2")

        picam = FakePicamera2.instances[-1]
        worker.configure(
            mode="video",
            main_w=1280,
            main_h=720,
            preview_w=226,
            preview_h=128,
            fps=30,
            zoom=1.0,
            ev=0.0,
            transform="auto",
        )
        self.assertEqual(picam.configured["type"], "video")
        self.assertEqual(picam.configured["lores"], {"size": (226, 128), "format": "YUV420"})
        self.assertEqual(picam.configured["transform"], FakeTransform(hflip=1, vflip=1))

        with tempfile.TemporaryDirectory() as tmpdir:
            video_path = os.path.join(tmpdir, ".VID_20261119_124000.mp4.part")
            res = worker.start_video(
                tmp_path=video_path,
                width=1280,
                height=720,
                fps=30,
                bitrate=4000000,
                audio=True,
            )
            self.assertTrue(res["audio_recorded"])
            self.assertTrue(picam.recording)
            self.assertIsInstance(picam.encoder, FakeH264Encoder)
            self.assertEqual(picam.encoder.bitrate, 4000000)
            self.assertIsInstance(picam.output, FakeFfmpegOutput)
            # Ruling 3: explicit MP4 muxer flags for .mp4.part staging path
            self.assertIn("-f mp4", picam.output.output_filename)
            self.assertIn("-movflags +faststart", picam.output.output_filename)
            self.assertIn(video_path, picam.output.output_filename)

            # Simulate lores callbacks firing during active recording with padded stride 256
            picam.emit_lores_frame(stride=256)
            picam.emit_lores_frame(stride=256)
            self.assertEqual(len(writer.frames), 2)
            w, h, pixfmt, flags, payload = writer.frames[-1]
            self.assertEqual((w, h, pixfmt, flags), (226, 128, PIXFMT_YUV420, FLAG_ASPECT_16_9))
            self.assertEqual(len(payload), 226 * 128 * 3 // 2)
            # Verify stride padding (0xFF bytes) was stripped out cleanly
            self.assertNotIn(255, payload)

            stop_res = worker.stop_video()
            self.assertFalse(picam.recording)
            self.assertTrue(stop_res["audio_recorded"])
            self.assertGreater(stop_res["bytes"], 0)
            self.assertGreaterEqual(stop_res["duration_sec"], 0.0)

    def test_audio_init_failure_falls_back_to_muted_and_emits_warn(self):
        FakePicamera2.fail_audio_on_start = True
        worker, _, events = self._make_worker()
        worker.start()
        worker.configure(
            mode="video",
            main_w=1280,
            main_h=720,
            preview_w=226,
            preview_h=128,
            fps=30,
            zoom=1.0,
            ev=0.0,
            transform="0",
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            video_path = os.path.join(tmpdir, ".VID_muted.mp4.part")
            res = worker.start_video(
                tmp_path=video_path,
                width=1280,
                height=720,
                fps=30,
                bitrate=4000000,
                audio=True,
            )
            self.assertFalse(res["audio_recorded"])

            picam = FakePicamera2.instances[-1]
            self.assertEqual(len(picam.recording_attempts), 2)
            self.assertTrue(picam.recording_attempts[0][1].audio)
            self.assertFalse(picam.recording_attempts[1][1].audio)
            self.assertTrue(picam.recording)

            warns = [e for e in events if e.get("event") == "warn"]
            self.assertEqual(len(warns), 1)
            self.assertEqual(warns[0]["code"], "audio_unavailable")

            stop_res = worker.stop_video()
            self.assertFalse(stop_res["audio_recorded"])

    def test_pause_and_resume_release_and_reacquire_camera(self):
        worker, _, _ = self._make_worker()
        worker.start()
        first_picam = FakePicamera2.instances[-1]

        worker.configure(
            mode="photo",
            main_w=3280,
            main_h=2464,
            preview_w=226,
            preview_h=170,
            fps=30,
            zoom=2.0,
            ev=0.5,
            transform="hflip",
        )
        self.assertTrue(first_picam.started)
        self.assertIn(
            {"ScalerCrop": (820, 616, 1640, 1232), "ExposureValue": 0.5},
            first_picam.controls_history,
        )

        worker.pause()
        self.assertFalse(first_picam.started)
        self.assertTrue(first_picam.closed)

        worker.resume()
        self.assertEqual(len(FakePicamera2.instances), 2)
        second_picam = FakePicamera2.instances[-1]
        self.assertTrue(second_picam.started)
        self.assertEqual(second_picam.configured["main"]["size"], (3280, 2464))
        self.assertEqual(second_picam.configured["lores"]["size"], (226, 170))
        self.assertEqual(second_picam.configured["transform"], FakeTransform(hflip=1, vflip=0))
        self.assertIn(
            {"ScalerCrop": (820, 616, 1640, 1232), "ExposureValue": 0.5},
            second_picam.controls_history,
        )

    def test_capture_still_uses_jpeg_format_for_tmp_extension(self):
        worker, _, _ = self._make_worker()
        worker.start()
        picam = FakePicamera2.instances[-1]
        worker.configure(
            mode="photo",
            main_w=3280,
            main_h=2464,
            preview_w=226,
            preview_h=170,
            fps=30,
            zoom=1.0,
            ev=0.0,
            transform="auto",
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            still_a = os.path.join(tmpdir, ".IMG_1.jpg.tmp")
            res_a = worker.capture_still(still_a, 3280, 2464)
            self.assertEqual(res_a["width"], 3280)
            self.assertEqual(res_a["height"], 2464)
            self.assertGreater(res_a["bytes"], 0)
            self.assertEqual(picam.captured_files[-1], (still_a, "main", "jpeg"))

            still_b = os.path.join(tmpdir, ".IMG_2.jpg.tmp")
            res_b = worker.capture_still(still_b, 1640, 1232)
            self.assertEqual(res_b["width"], 1640)
            self.assertEqual(res_b["height"], 1232)
            self.assertEqual(picam.captured_files[-1], (still_b, "main", "jpeg"))


if __name__ == "__main__":
    unittest.main()
