import os
import stat
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGING_DIR = ROOT / "packaging"


def _parse_png_chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Invalid PNG signature")
    pos = 8
    chunks: list[tuple[bytes, bytes]] = []
    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        chunk_type = data[pos + 4 : pos + 8]
        chunk_data = data[pos + 8 : pos + 8 + length]
        crc_expected = struct.unpack(
            ">I", data[pos + 8 + length : pos + 12 + length]
        )[0]
        crc_actual = zlib.crc32(chunk_type + chunk_data) & 0xFFFFFFFF
        if crc_expected != crc_actual:
            raise ValueError(f"CRC mismatch in chunk {chunk_type!r}")
        chunks.append((chunk_type, chunk_data))
        pos += 12 + length
        if chunk_type == b"IEND":
            break
    return chunks


class TestPackaging(unittest.TestCase):
    def test_debian_control(self) -> None:
        ctrl = (PACKAGING_DIR / "debian" / "control").read_text(encoding="utf-8")
        self.assertIn("Package: czcamera", ctrl)
        self.assertIn("Version: 1.0.0-m5stack1", ctrl)
        self.assertIn("Architecture: arm64", ctrl)
        self.assertIn("Depends:", ctrl)
        for dep in (
            "python3",
            "python3-picamera2",
            "ffmpeg",
            "alsa-utils",
            "libjpeg62-turbo | libjpeg8",
            "libfreetype6",
            "libc6",
            "libstdc++6",
        ):
            self.assertIn(dep, ctrl)
        self.assertNotIn("cp0-sync", ctrl)
        self.assertNotIn("libcamera0", ctrl)
        self.assertNotIn("${", ctrl)

    def test_desktop_entry_and_icon_80x80(self) -> None:
        desktop = (PACKAGING_DIR / "czcamera.desktop").read_text(encoding="utf-8")
        self.assertIn("Type=Application", desktop)
        self.assertIn("Name=czcamera", desktop)
        self.assertIn("Exec=/usr/bin/czcamera", desktop)
        self.assertIn(
            "Icon=/usr/share/APPLaunch/share/images/czcamera.png", desktop
        )
        self.assertIn("Terminal=false", desktop)
        self.assertIn("Categories=AudioVideo;Photography;", desktop)

        png = (PACKAGING_DIR / "czcamera.png").read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        width, height, bit_depth, color_type = struct.unpack(">IIBB", png[16:26])
        self.assertEqual(width, 80)
        self.assertEqual(height, 80)
        self.assertEqual(bit_depth, 8)
        self.assertIn(color_type, (2, 6))

        chunks = _parse_png_chunks(png)
        chunk_types = [t for t, _ in chunks]
        self.assertIn(b"IHDR", chunk_types)
        self.assertIn(b"IDAT", chunk_types)
        self.assertEqual(chunk_types[-1], b"IEND")

        idat_payload = b"".join(d for t, d in chunks if t == b"IDAT")
        raw_pixels = zlib.decompress(idat_payload)
        channels = 4 if color_type == 6 else 3
        self.assertEqual(len(raw_pixels), 80 * (1 + 80 * channels))

    def test_maintainer_scripts_and_dockerfile_arm64(self) -> None:
        postinst_path = PACKAGING_DIR / "debian" / "postinst"
        prerm_path = PACKAGING_DIR / "debian" / "prerm"

        for script_path in (postinst_path, prerm_path):
            mode = script_path.stat().st_mode
            self.assertEqual(
                stat.S_IMODE(mode) & 0o755,
                0o755,
                f"{script_path} must be executable",
            )
            syntax = subprocess.run(
                ["sh", "-n", str(script_path)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                syntax.returncode, 0, f"sh -n failed on {script_path}: {syntax.stderr}"
            )
            text = script_path.read_text(encoding="utf-8")
            self.assertIn("set -e", text)
            self.assertNotIn("systemctl", text)
            self.assertNotIn("cp0-sync", text)

        prerm = prerm_path.read_text(encoding="utf-8")
        self.assertIn("/usr/lib/czcamera", prerm)
        self.assertIn("__pycache__", prerm)
        # Ensure recursive cleanup (find ... or rm -rf ...)
        self.assertTrue(
            "find /usr/lib/czcamera" in prerm or "rm -rf" in prerm,
            "prerm must recursively clean __pycache__ under /usr/lib/czcamera",
        )

        dockerfile = (PACKAGING_DIR / "Dockerfile.arm64").read_text(
            encoding="utf-8"
        )
        self.assertIn("--platform=linux/arm64", dockerfile)
        self.assertIn("debian:bookworm-slim", dockerfile)
        for pkg in (
            "build-essential",
            "cmake",
            "pkg-config",
            "libjpeg62-turbo-dev",
            "libfreetype-dev",
            "dpkg-dev",
            "python3",
            "ffmpeg",
        ):
            self.assertIn(pkg, dockerfile)
        self.assertNotIn("libcamera-dev", dockerfile)
        self.assertNotIn("libsqlite3-dev", dockerfile)
        self.assertIn("-DCZ_BUILD_APP=ON", dockerfile)
        self.assertIn("-DCZ_TARGET_DEVICE=ON", dockerfile)
        self.assertIn("-DCZ_BUILD_TESTS=ON", dockerfile)
        self.assertIn("ctest", dockerfile)
        self.assertIn("cam-helper/tests", dockerfile)
        self.assertIn("build-deb.sh --local", dockerfile)

    def test_build_deb_stage_only(self) -> None:
        build_script = PACKAGING_DIR / "build-deb.sh"
        mode = build_script.stat().st_mode
        self.assertEqual(
            stat.S_IMODE(mode) & 0o755,
            0o755,
            "build-deb.sh must be executable",
        )

        syntax = subprocess.run(
            ["bash", "-n", str(build_script)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            syntax.returncode, 0, f"bash -n failed: {syntax.stderr}"
        )

        script_text = build_script.read_text(encoding="utf-8")
        self.assertIn("czcamera_1.0.0_m5stack1_arm64.deb", script_text)
        self.assertIn("--bin", script_text)
        self.assertIn("--ui-bin", script_text)

        for flag in ("--ui-bin", "--bin"):
            with tempfile.TemporaryDirectory() as tmp:
                tmp_path = Path(tmp)
                dummy_bin = tmp_path / "czcamera"
                dummy_bin.write_bytes(b"#!/bin/sh\nexit 0\n")
                dummy_bin.chmod(0o755)

                stage_dir = tmp_path / "stage"
                res = subprocess.run(
                    [
                        str(build_script),
                        "--stage-only",
                        str(stage_dir),
                        flag,
                        str(dummy_bin),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(
                    res.returncode,
                    0,
                    f"--stage-only with {flag} failed:\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}",
                )

                expected_files = [
                    stage_dir / "DEBIAN" / "control",
                    stage_dir / "DEBIAN" / "postinst",
                    stage_dir / "DEBIAN" / "prerm",
                    stage_dir / "usr" / "bin" / "czcamera",
                    stage_dir / "usr" / "bin" / "czcamera-cam",
                    stage_dir / "usr" / "lib" / "czcamera" / "czcamera_cam" / "__init__.py",
                    stage_dir / "usr" / "lib" / "czcamera" / "czcamera_cam" / "protocol.py",
                    stage_dir / "usr" / "lib" / "czcamera" / "czcamera_cam" / "picam2_worker.py",
                    stage_dir / "usr" / "share" / "APPLaunch" / "applications" / "czcamera.desktop",
                    stage_dir / "usr" / "share" / "APPLaunch" / "share" / "images" / "czcamera.png",
                ]
                for fpath in expected_files:
                    self.assertTrue(fpath.is_file(), f"Missing staged file: {fpath}")

                exec_paths = {
                    stage_dir / "DEBIAN" / "postinst",
                    stage_dir / "DEBIAN" / "prerm",
                    stage_dir / "usr" / "bin" / "czcamera",
                    stage_dir / "usr" / "bin" / "czcamera-cam",
                }
                for exec_path in exec_paths:
                    self.assertEqual(
                        stat.S_IMODE(exec_path.stat().st_mode),
                        0o755,
                        f"Expected 0755 on {exec_path}",
                    )
                for non_exec_path in set(expected_files) - exec_paths:
                    self.assertEqual(
                        stat.S_IMODE(non_exec_path.stat().st_mode),
                        0o644,
                        f"Expected 0644 on {non_exec_path}",
                    )

                pycache_dirs = list(
                    (stage_dir / "usr" / "lib" / "czcamera").rglob("__pycache__")
                )
                self.assertEqual(
                    pycache_dirs, [], "Staged Python tree must not contain __pycache__"
                )
                self.assertFalse(
                    (stage_dir / "usr" / "lib" / "systemd").exists(),
                    "czcamera must not install systemd units",
                )


if __name__ == "__main__":
    unittest.main()
