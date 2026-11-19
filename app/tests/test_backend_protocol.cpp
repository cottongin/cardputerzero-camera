#include "camera_backend.hpp"
#include "jpeg_pipeline.hpp"
#include "sim_backend.hpp"
#include "subprocess_backend.hpp"

#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TempDir {
  std::string path;
  explicit TempDir(const std::string& name) {
    const std::string base =
        "/tmp/czcamera_test_" + name + "_" + std::to_string(::getpid());
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    assert(!ec);
    path = base;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

struct EnvVarGuard {
  std::string key;
  bool had_old = false;
  std::string old_val;

  EnvVarGuard(const std::string& k, const std::string& v) : key(k) {
    const char* prev = std::getenv(key.c_str());
    if (prev != nullptr) {
      had_old = true;
      old_val = prev;
    }
    ::setenv(key.c_str(), v.c_str(), 1);
  }

  explicit EnvVarGuard(const std::string& k) : key(k) {
    const char* prev = std::getenv(key.c_str());
    if (prev != nullptr) {
      had_old = true;
      old_val = prev;
    }
    ::unsetenv(key.c_str());
  }

  ~EnvVarGuard() {
    if (had_old) {
      ::setenv(key.c_str(), old_val.c_str(), 1);
    } else {
      ::unsetenv(key.c_str());
    }
  }
};

std::array<uint8_t, 16> pack_header(const char magic[4],
                                    uint16_t w,
                                    uint16_t h,
                                    uint8_t pixfmt,
                                    uint8_t flags,
                                    uint16_t reserved,
                                    uint32_t payload_bytes) {
  std::array<uint8_t, 16> hdr{};
  std::memcpy(hdr.data(), magic, 4);
  hdr[4] = static_cast<uint8_t>(w & 0xFFu);
  hdr[5] = static_cast<uint8_t>((w >> 8) & 0xFFu);
  hdr[6] = static_cast<uint8_t>(h & 0xFFu);
  hdr[7] = static_cast<uint8_t>((h >> 8) & 0xFFu);
  hdr[8] = pixfmt;
  hdr[9] = flags;
  hdr[10] = static_cast<uint8_t>(reserved & 0xFFu);
  hdr[11] = static_cast<uint8_t>((reserved >> 8) & 0xFFu);
  hdr[12] = static_cast<uint8_t>(payload_bytes & 0xFFu);
  hdr[13] = static_cast<uint8_t>((payload_bytes >> 8) & 0xFFu);
  hdr[14] = static_cast<uint8_t>((payload_bytes >> 16) & 0xFFu);
  hdr[15] = static_cast<uint8_t>((payload_bytes >> 24) & 0xFFu);
  return hdr;
}

double mean_luma(const std::vector<uint8_t>& rgb24) {
  if (rgb24.empty()) return 0.0;
  double sum = 0.0;
  const size_t pixels = rgb24.size() / 3;
  for (size_t i = 0; i < pixels; ++i) {
    sum += 0.299 * rgb24[i * 3 + 0] + 0.587 * rgb24[i * 3 + 1] +
           0.114 * rgb24[i * 3 + 2];
  }
  return sum / static_cast<double>(pixels);
}

bool wait_for_preview_frame(cz::ICameraBackend& backend,
                            uint64_t after_seq,
                            cz::PreviewFrame& out,
                            int timeout_ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (backend.copy_latest_preview(after_seq, out)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

void test_czfr_header_and_yuv420_to_rgb24() {
  uint16_t w = 0;
  uint16_t h = 0;
  uint8_t pixfmt = 0;
  uint8_t flags = 0;
  uint32_t payload = 0;

  // Valid YUV420 226x170 header
  const uint32_t yuv_len = 226u * 170u * 3u / 2u;
  const auto valid_hdr = pack_header("CZFR", 226, 170, 1, 0, 0, yuv_len);
  assert(cz::parse_czfr_header(valid_hdr.data(), w, h, pixfmt, flags, payload));
  assert(w == 226 && h == 170 && pixfmt == 1 && flags == 0 &&
         payload == yuv_len);

  // Valid RGB24 16:9 header at exact 16 MiB boundary
  const uint32_t max_ok = 16u * 1024u * 1024u;
  const auto max_hdr = pack_header("CZFR", 226, 128, 2, 1, 0, max_ok);
  assert(cz::parse_czfr_header(max_hdr.data(), w, h, pixfmt, flags, payload));
  assert(w == 226 && h == 128 && pixfmt == 2 && flags == 1 &&
         payload == max_ok);

  // Reject nullptr, bad magic, zero dims, invalid pixfmt, zero payload, >16 MiB
  assert(!cz::parse_czfr_header(nullptr, w, h, pixfmt, flags, payload));

  const auto bad_magic = pack_header("BAD!", 226, 170, 1, 0, 0, yuv_len);
  assert(!cz::parse_czfr_header(bad_magic.data(), w, h, pixfmt, flags, payload));

  const auto zero_w = pack_header("CZFR", 0, 170, 1, 0, 0, yuv_len);
  assert(!cz::parse_czfr_header(zero_w.data(), w, h, pixfmt, flags, payload));

  const auto bad_fmt = pack_header("CZFR", 226, 170, 9, 0, 0, yuv_len);
  assert(!cz::parse_czfr_header(bad_fmt.data(), w, h, pixfmt, flags, payload));

  const auto zero_len = pack_header("CZFR", 226, 170, 1, 0, 0, 0);
  assert(!cz::parse_czfr_header(zero_len.data(), w, h, pixfmt, flags, payload));

  const auto oversized = pack_header("CZFR", 226, 170, 1, 0, 0, max_ok + 1u);
  assert(!cz::parse_czfr_header(oversized.data(), w, h, pixfmt, flags, payload));

  // YUV420 -> RGB24 validation and BT.601 color accuracy
  std::vector<uint8_t> rgb;
  assert(!cz::yuv420_to_rgb24(nullptr, 4, 4, rgb));
  std::vector<uint8_t> dummy(16, 0);
  assert(!cz::yuv420_to_rgb24(dummy.data(), 0, 4, rgb));
  assert(!cz::yuv420_to_rgb24(dummy.data(), 3, 4, rgb));
  assert(!cz::yuv420_to_rgb24(dummy.data(), 4, 3, rgb));

  // 4x4 YUV420 image with 4 distinct 2x2 quadrants:
  // Top-left:     Black (Y=16,  U=128, V=128) -> (0, 0, 0)
  // Top-right:    White (Y=235, U=128, V=128) -> (255, 255, 255)
  // Bottom-left:  Red   (Y=81,  U=90,  V=240) -> (~255, ~0, ~0)
  // Bottom-right: Blue  (Y=41,  U=240, V=110) -> (~0, ~0, ~255)
  // Plus test Green separately.
  std::vector<uint8_t> yuv(4 * 4 * 3 / 2, 0);
  // Y plane (4x4)
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      uint8_t y_val = 16;
      if (row < 2 && col < 2) y_val = 16;
      else if (row < 2 && col >= 2) y_val = 235;
      else if (row >= 2 && col < 2) y_val = 81;
      else y_val = 41;
      yuv[static_cast<size_t>(row * 4 + col)] = y_val;
    }
  }
  // U plane (2x2 at offset 16): TL=128, TR=128, BL=90, BR=240
  yuv[16] = 128;
  yuv[17] = 128;
  yuv[18] = 90;
  yuv[19] = 240;
  // V plane (2x2 at offset 20): TL=128, TR=128, BL=240, BR=110
  yuv[20] = 128;
  yuv[21] = 128;
  yuv[22] = 240;
  yuv[23] = 110;

  assert(cz::yuv420_to_rgb24(yuv.data(), 4, 4, rgb));
  assert(rgb.size() == 4u * 4u * 3u);

  auto px = [&](int x, int y) -> std::array<uint8_t, 3> {
    const size_t idx = static_cast<size_t>(y * 4 + x) * 3u;
    return {rgb[idx], rgb[idx + 1], rgb[idx + 2]};
  };

  // Black (TL)
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      auto c = px(x, y);
      assert(c[0] <= 2 && c[1] <= 2 && c[2] <= 2);
    }
  }
  // White (TR)
  for (int y = 0; y < 2; ++y) {
    for (int x = 2; x < 4; ++x) {
      auto c = px(x, y);
      assert(c[0] >= 253 && c[1] >= 253 && c[2] >= 253);
    }
  }
  // Red (BL)
  for (int y = 2; y < 4; ++y) {
    for (int x = 0; x < 2; ++x) {
      auto c = px(x, y);
      assert(c[0] >= 248 && c[1] <= 8 && c[2] <= 8);
    }
  }
  // Blue (BR)
  for (int y = 2; y < 4; ++y) {
    for (int x = 2; x < 4; ++x) {
      auto c = px(x, y);
      assert(c[0] <= 8 && c[1] <= 8 && c[2] >= 248);
    }
  }

  // Pure green 2x2 block: Y=145, U=54, V=34
  std::vector<uint8_t> green_yuv = {145, 145, 145, 145, 54, 34};
  assert(cz::yuv420_to_rgb24(green_yuv.data(), 2, 2, rgb));
  assert(rgb.size() == 12);
  assert(rgb[0] <= 8 && rgb[1] >= 248 && rgb[2] <= 8);
}

void test_sim_backend_lifecycle() {
  TempDir tmp("sim_backend");

  // Factory selection tests
  {
    auto forced = cz::create_camera_backend(true);
    assert(forced != nullptr);
    assert(forced->status().backend_name == "sim");

    EnvVarGuard env_b("CZCAMERA_BACKEND", "sim");
    auto from_env = cz::create_camera_backend(false);
    assert(from_env != nullptr);
    assert(from_env->status().backend_name == "sim");
  }
  {
    EnvVarGuard clear_backend("CZCAMERA_BACKEND");
    EnvVarGuard clear_bin("CZCAMERA_CAM_BIN");
    auto default_backend = cz::create_camera_backend(false);
    assert(default_backend != nullptr);
#ifdef CZ_TARGET_DEVICE
    assert(default_backend->status().backend_name == "picamera2-subprocess");
#else
    assert(default_backend->status().backend_name == "sim");
#endif
  }

  cz::SimCameraBackend sim;
  assert(!sim.status().ready);
  assert(sim.status().backend_name == "sim");

  cz::CameraStreamSpec spec;
  spec.mode = "photo";
  spec.main_width = 640;
  spec.main_height = 480;
  spec.preview_width = 226;
  spec.preview_height = 170;
  spec.fps = 30;
  spec.zoom = 1.0f;
  spec.ev = 0.0f;
  spec.camera_transform = "auto";

  std::string err;
  assert(sim.start(spec, err));
  assert(err.empty());
  assert(sim.status().ready);
  assert(!sim.status().paused);
  assert(!sim.status().recording);

  cz::PreviewFrame f1;
  assert(wait_for_preview_frame(sim, 0, f1));
  assert(f1.seq >= 1);
  assert(f1.width == 226 && f1.height == 170);
  assert(f1.rgb24.size() == 226u * 170u * 3u);

  // Next frame should arrive with higher sequence number
  cz::PreviewFrame f2;
  assert(wait_for_preview_frame(sim, f1.seq, f2));
  assert(f2.seq > f1.seq);
  assert(!sim.copy_latest_preview(f2.seq, f2));

  // EV and zoom controls visibly affect frame brightness/content
  assert(sim.set_controls(1.0f, -2.0f, err));
  cz::PreviewFrame dark_frame;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  assert(wait_for_preview_frame(sim, f2.seq, dark_frame));

  assert(sim.set_controls(3.0f, 2.0f, err));
  cz::PreviewFrame bright_frame;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  assert(wait_for_preview_frame(sim, dark_frame.seq, bright_frame));
  assert(mean_luma(bright_frame.rgb24) > mean_luma(dark_frame.rgb24) + 15.0);

  // Transform flip ("180") flips sky (top) and lake (bottom) vertically
  cz::CameraStreamSpec normal_spec = spec;
  normal_spec.camera_transform = "0";
  assert(sim.reconfigure(normal_spec, err));
  cz::PreviewFrame norm_f;
  assert(wait_for_preview_frame(sim, bright_frame.seq, norm_f));
  const double norm_top_b = norm_f.rgb24[2];  // sky blue at (0,0)
  const double norm_bot_b =
      norm_f.rgb24[(static_cast<size_t>(norm_f.height - 1) * norm_f.width) * 3 + 2];

  cz::CameraStreamSpec flip_spec = spec;
  flip_spec.camera_transform = "180";
  assert(sim.reconfigure(flip_spec, err));
  cz::PreviewFrame flip_f;
  assert(wait_for_preview_frame(sim, norm_f.seq, flip_f));
  const double flip_top_b = flip_f.rgb24[2];
  const double flip_bot_b =
      flip_f.rgb24[(static_cast<size_t>(flip_f.height - 1) * flip_f.width) * 3 + 2];
  assert(norm_top_b != flip_top_b || norm_bot_b != flip_bot_b);
  assert(std::abs(norm_top_b - flip_bot_b) < 2.0);

  // Still capture writes a valid JPEG
  const std::string jpg_tmp = tmp.path + "/.IMG_test.jpg.tmp";
  cz::StillCaptureResult still = sim.capture_still(jpg_tmp, 640, 480);
  assert(still.ok);
  assert(still.width == 640 && still.height == 480);
  assert(still.bytes > 0);
  cz::RgbImage decoded;
  assert(cz::decode_jpeg_rgb24(jpg_tmp, decoded, err));
  assert(decoded.width == 640 && decoded.height == 480);

  // Pause & resume toggle paused state
  assert(sim.pause(err));
  assert(sim.status().paused);
  assert(sim.resume(err));
  assert(!sim.status().paused);

  // Video start/stop writes non-empty .part and returns positive duration
  cz::VideoSpec vspec;
  vspec.width = 640;
  vspec.height = 360;
  vspec.fps = 15;
  vspec.bitrate_bps = 2000000;
  vspec.audio_enabled = false;
  vspec.tmp_path = tmp.path + "/.VID_test.mp4.part";

  assert(sim.start_video(vspec, err));
  assert(sim.status().recording);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  cz::VideoStopResult vstop = sim.stop_video();
  assert(vstop.ok);
  assert(vstop.duration_sec > 0.0);
  assert(vstop.bytes > 0);
  assert(!sim.status().recording);

  struct stat st {};
  assert(::stat(vspec.tmp_path.c_str(), &st) == 0);
  assert(st.st_size > 0);

  sim.stop();
  assert(!sim.status().ready);
}

void test_subprocess_backend_with_mock_driver() {
  TempDir tmp("subproc_mock");
  const std::string cam_bin =
      std::string(CZ_SOURCE_DIR) + "/cam-helper/bin/czcamera-cam";
  const std::string log_path = tmp.path + "/logs/czcamera.log";

  EnvVarGuard env_bin("CZCAMERA_CAM_BIN", cam_bin);
  EnvVarGuard env_log("CZCAMERA_LOG", log_path);
  EnvVarGuard env_backend("CZCAMERA_BACKEND", "subprocess");

  // Factory honors CZCAMERA_BACKEND=subprocess + CZCAMERA_CAM_BIN
  {
    auto factory_backend = cz::create_camera_backend(false);
    assert(factory_backend != nullptr);
    assert(factory_backend->status().backend_name == "picamera2-subprocess");
  }

  cz::SubprocessCameraBackend backend("", {"--mock-driver"});
  assert(!backend.status().ready);

  cz::CameraStreamSpec spec;
  spec.mode = "photo";
  spec.main_width = 1640;
  spec.main_height = 1232;
  spec.preview_width = 226;
  spec.preview_height = 170;
  spec.fps = 30;
  spec.zoom = 1.0f;
  spec.ev = 0.0f;
  spec.camera_transform = "auto";

  std::string err;
  assert(backend.start(spec, err));
  assert(err.empty());
  assert(backend.status().ready);
  assert(backend.status().backend_name == "picamera2-subprocess");
  assert(backend.child_pid() > 0);

  // Receive live PreviewFrames over CZFR pipe
  cz::PreviewFrame f1;
  assert(wait_for_preview_frame(backend, 0, f1));
  assert(f1.width == 226 && f1.height == 170);
  assert(f1.rgb24.size() == 226u * 170u * 3u);
  assert(!f1.is_16_9);

  cz::PreviewFrame f2;
  assert(wait_for_preview_frame(backend, f1.seq, f2));
  assert(f2.seq > f1.seq);

  // Live controls
  assert(backend.set_controls(2.0f, -0.5f, err));

  // Capture still via helper -> valid JPEG decodable by libjpeg
  const std::string jpg_tmp = tmp.path + "/.IMG_mock.jpg.tmp";
  cz::StillCaptureResult still = backend.capture_still(jpg_tmp, 640, 480);
  assert(still.ok);
  assert(still.width == 640 && still.height == 480);
  assert(still.bytes > 0);
  cz::RgbImage img;
  assert(cz::decode_jpeg_rgb24(jpg_tmp, img, err));
  assert(img.width == 640 && img.height == 480);

  // Start/stop video
  cz::VideoSpec vspec;
  vspec.width = 1280;
  vspec.height = 720;
  vspec.fps = 30;
  vspec.bitrate_bps = 4000000;
  vspec.audio_enabled = true;
  vspec.tmp_path = tmp.path + "/.VID_mock.mp4.part";

  assert(backend.start_video(vspec, err));
  assert(backend.status().recording);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  cz::VideoStopResult vstop = backend.stop_video();
  assert(vstop.ok);
  assert(vstop.duration_sec > 0.0);
  assert(vstop.bytes > 0);
  assert(vstop.audio_recorded);
  assert(!backend.status().recording);

  // Reconfigure back to 16:9 preview + explicit transform and wait for 226x128
  cz::CameraStreamSpec spec16;
  spec16.mode = "video";
  spec16.main_width = 1280;
  spec16.main_height = 720;
  spec16.preview_width = 226;
  spec16.preview_height = 128;
  spec16.fps = 30;
  spec16.zoom = 1.5f;
  spec16.ev = 0.5f;
  spec16.camera_transform = "180";
  assert(backend.reconfigure(spec16, err));

  cz::PreviewFrame f16;
  uint64_t seen_seq = f2.seq;
  bool got_16_9 = false;
  const auto reconf_dl =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < reconf_dl) {
    if (backend.copy_latest_preview(seen_seq, f16)) {
      seen_seq = f16.seq;
      if (f16.width == 226 && f16.height == 128 && f16.is_16_9) {
        got_16_9 = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(got_16_9);
  assert(f16.rgb24.size() == 226u * 128u * 3u);

  // Pause & resume
  assert(backend.pause(err));
  assert(backend.status().paused);
  assert(backend.resume(err));
  assert(!backend.status().paused);

  cz::PreviewFrame f_resumed;
  assert(wait_for_preview_frame(backend, f16.seq, f_resumed));
  assert(f_resumed.seq > f16.seq);

  const pid_t pid = backend.child_pid();
  backend.stop();
  assert(!backend.status().ready);
  assert(backend.child_pid() == -1);
  int st = 0;
  assert(::waitpid(pid, &st, WNOHANG) == -1 && errno == ECHILD);
  assert(fs::exists(fs::path(log_path).parent_path()));
}

void test_subprocess_backend_crash_recovery_no_deadlock() {
  TempDir tmp("subproc_crash");
  const std::string cam_bin =
      std::string(CZ_SOURCE_DIR) + "/cam-helper/bin/czcamera-cam";
  const std::string log_path = tmp.path + "/czcamera.log";
  EnvVarGuard env_log("CZCAMERA_LOG", log_path);

  // Part A: SIGKILL mid-stream against real czcamera-cam --mock-driver
  {
    cz::SubprocessCameraBackend backend(cam_bin, {"--mock-driver"});
    cz::CameraStreamSpec spec;
    spec.preview_width = 226;
    spec.preview_height = 170;
    std::string err;
    assert(backend.start(spec, err));

    cz::PreviewFrame f1;
    assert(wait_for_preview_frame(backend, 0, f1));

    const pid_t crashed_pid = backend.child_pid();
    assert(crashed_pid > 0);
    assert(::kill(crashed_pid, SIGKILL) == 0);

    // Wait briefly for stdout reader thread to observe EOF and reap child
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (backend.status().ready &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const cz::BackendHealth health = backend.status();
    assert(!health.ready);
    assert(!health.last_error.empty());

    // Subsequent calls return cleanly with ok=false without SIGPIPE or deadlock
    const cz::StillCaptureResult still =
        backend.capture_still(tmp.path + "/.dead.jpg.tmp", 640, 480);
    assert(!still.ok);
    assert(!still.error.empty());

    assert(!backend.set_controls(2.0f, 0.0f, err));
    assert(!err.empty());

    // Ensure crashed PID was reaped (no zombie)
    int st = 0;
    assert(::waitpid(crashed_pid, &st, WNOHANG) == -1 && errno == ECHILD);

    // Restarting spawns a fresh worker cleanly
    assert(backend.start(spec, err));
    assert(backend.status().ready);
    const pid_t fresh_pid = backend.child_pid();
    assert(fresh_pid > 0 && fresh_pid != crashed_pid);

    cz::PreviewFrame f_fresh;
    assert(wait_for_preview_frame(backend, 0, f_fresh));
    assert(f_fresh.width == 226 && f_fresh.height == 170);

    backend.stop();
    assert(::waitpid(fresh_pid, &st, WNOHANG) == -1 && errno == ECHILD);
  }

  // Part B: SIGKILL mid-command while capture_still() is blocked waiting on reply
  {
    const std::string stall_helper = tmp.path + "/stall_helper.py";
    {
      std::ofstream out(stall_helper);
      assert(out.is_open());
      out << "#!/usr/bin/python3\n"
          << "import json, sys, time\n"
          << "sys.stdout.write(json.dumps({'event':'ready','protocol':1,'backend':'mock','has_audio':False}) + '\\n')\n"
          << "sys.stdout.flush()\n"
          << "for line in sys.stdin:\n"
          << "    msg = json.loads(line)\n"
          << "    if msg.get('cmd') == 'capture_still':\n"
          << "        time.sleep(30.0)\n"
          << "    else:\n"
          << "        sys.stdout.write(json.dumps({'id': msg.get('id'), 'ok': True}) + '\\n')\n"
          << "        sys.stdout.flush()\n";
    }
    assert(::chmod(stall_helper.c_str(), 0755) == 0);

    cz::SubprocessCameraBackend backend(stall_helper);
    cz::CameraStreamSpec spec;
    std::string err;
    assert(backend.start(spec, err));
    const pid_t stalled_pid = backend.child_pid();
    assert(stalled_pid > 0);

    // Trigger SIGKILL shortly after capture_still starts waiting
    std::thread killer([stalled_pid]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      ::kill(stalled_pid, SIGKILL);
    });

    const auto t0 = std::chrono::steady_clock::now();
    const cz::StillCaptureResult still =
        backend.capture_still(tmp.path + "/.hung.jpg.tmp", 640, 480);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();
    killer.join();

    assert(!still.ok);
    assert(!still.error.empty());
    assert(elapsed_ms < 2000);  // Must wake immediately, well before 5s timeout!
    assert(!backend.status().ready);
    backend.stop();
  }

  // Part C: PIXFMT_RGB24 framing + warn("audio_unavailable") event + missing binary
  {
    const std::string rgb_warn_helper = tmp.path + "/rgb_warn_helper.py";
    {
      std::ofstream out(rgb_warn_helper);
      assert(out.is_open());
      out << "#!/usr/bin/python3\n"
          << "import argparse, json, os, struct, sys\n"
          << "p = argparse.ArgumentParser()\n"
          << "p.add_argument('--frame-fd', type=int, required=True)\n"
          << "args = p.parse_args()\n"
          << "sys.stdout.write(json.dumps({'event':'ready','protocol':1,'backend':'mock','has_audio':False}) + '\\n')\n"
          << "sys.stdout.flush()\n"
          << "for line in sys.stdin:\n"
          << "    msg = json.loads(line)\n"
          << "    cmd = msg.get('cmd')\n"
          << "    if cmd == 'configure':\n"
          << "        w, h = int(msg['preview_w']), int(msg['preview_h'])\n"
          << "        rgb = bytes([12, 34, 56]) * (w * h)\n"
          << "        hdr = struct.pack('<4sHHBBHI', b'CZFR', w, h, 2, 0, 0, len(rgb))\n"
          << "        os.write(args.frame_fd, hdr + rgb)\n"
          << "        sys.stdout.write(json.dumps({'id': msg['id'], 'ok': True}) + '\\n')\n"
          << "    elif cmd == 'start_video':\n"
          << "        sys.stdout.write(json.dumps({'event':'warn','code':'audio_unavailable','message':'ALSA muted fallback'}) + '\\n')\n"
          << "        sys.stdout.write(json.dumps({'id': msg['id'], 'ok': True, 'audio_recorded': False}) + '\\n')\n"
          << "    elif cmd == 'shutdown':\n"
          << "        sys.stdout.write(json.dumps({'id': msg['id'], 'ok': True}) + '\\n')\n"
          << "        sys.stdout.flush()\n"
          << "        break\n"
          << "    else:\n"
          << "        sys.stdout.write(json.dumps({'id': msg['id'], 'ok': True}) + '\\n')\n"
          << "    sys.stdout.flush()\n";
    }
    assert(::chmod(rgb_warn_helper.c_str(), 0755) == 0);

    cz::SubprocessCameraBackend backend(rgb_warn_helper);
    cz::CameraStreamSpec spec;
    spec.preview_width = 226;
    spec.preview_height = 170;
    std::string err;
    assert(backend.start(spec, err));

    cz::PreviewFrame rgb_frame;
    assert(wait_for_preview_frame(backend, 0, rgb_frame));
    assert(rgb_frame.width == 226 && rgb_frame.height == 170);
    assert(rgb_frame.rgb24.size() == 226u * 170u * 3u);
    assert(rgb_frame.rgb24[0] == 12 && rgb_frame.rgb24[1] == 34 &&
           rgb_frame.rgb24[2] == 56);

    cz::VideoSpec vspec;
    vspec.width = 1280;
    vspec.height = 720;
    vspec.fps = 30;
    vspec.audio_enabled = true;
    vspec.tmp_path = tmp.path + "/.VID_warn.mp4.part";
    assert(backend.start_video(vspec, err));
    const cz::BackendHealth h = backend.status();
    assert(h.recording);
    assert(h.audio_warning);
    assert(h.last_error == "ALSA muted fallback");
    backend.stop();

    // Missing helper binary fails fast without waiting for 5s ready timeout
    cz::SubprocessCameraBackend missing("/nonexistent/czcamera_missing_helper");
    const auto t0 = std::chrono::steady_clock::now();
    assert(!missing.start(spec, err));
    const auto miss_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();
    assert(!err.empty());
    assert(!missing.status().ready);
    assert(miss_ms < 2000);
  }
}

}  // namespace

int main() {
  test_czfr_header_and_yuv420_to_rgb24();
  test_sim_backend_lifecycle();
  test_subprocess_backend_with_mock_driver();
  test_subprocess_backend_crash_recovery_no_deadlock();
  std::cout << "All test_backend_protocol tests passed!\n";
  return 0;
}
