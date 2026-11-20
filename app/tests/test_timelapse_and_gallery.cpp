#include "backlight.hpp"
#include "config.hpp"
#include "gallery_model.hpp"
#include "jpeg_pipeline.hpp"
#include "sim_backend.hpp"
#include "storage.hpp"
#include "timelapse_engine.hpp"
#include "video_player.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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
        "/tmp/czcamera_test_t5_" + name + "_" + std::to_string(::getpid());
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

void write_text_file(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  assert(out.is_open());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  assert(out.good());
}

std::string read_text_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  assert(in.is_open());
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

bool create_sample_jpeg(const std::string& path, int w, int h, uint8_t seed) {
  std::vector<uint8_t> rgb(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t idx = (static_cast<size_t>(y) * w + x) * 3u;
      rgb[idx + 0] = static_cast<uint8_t>((x * 3 + seed) & 0xFF);
      rgb[idx + 1] = static_cast<uint8_t>((y * 5 + seed * 2) & 0xFF);
      rgb[idx + 2] = static_cast<uint8_t>(((x + y) * 2 + seed * 3) & 0xFF);
    }
  }
  std::string err;
  return cz::encode_jpeg_rgb24(path, rgb.data(), w, h, 85, err);
}

std::string find_tool_path(const char* name) {
  const char* path_env = std::getenv("PATH");
  std::vector<std::string> dirs;
  if (path_env && *path_env) {
    std::string p(path_env);
    size_t start = 0;
    while (start <= p.size()) {
      const size_t colon = p.find(':', start);
      const std::string d =
          (colon == std::string::npos) ? p.substr(start) : p.substr(start, colon - start);
      if (!d.empty()) dirs.push_back(d);
      if (colon == std::string::npos) break;
      start = colon + 1;
    }
  }
  dirs.push_back("/usr/local/bin");
  dirs.push_back("/opt/homebrew/bin");
  dirs.push_back("/usr/bin");
  dirs.push_back("/bin");
  for (const auto& d : dirs) {
    const std::string cand = d + "/" + name;
    if (::access(cand.c_str(), X_OK) == 0) {
      return cand;
    }
  }
  return name;
}

bool run_ffmpeg_sync(const std::vector<std::string>& args) {
  const std::string exe = find_tool_path("ffmpeg");
  std::vector<std::string> full_args;
  full_args.push_back(exe);
  full_args.insert(full_args.end(), args.begin(), args.end());

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(full_args.size() + 1);
  for (auto& a : full_args) {
    argv_ptrs.push_back(const_cast<char*>(a.c_str()));
  }
  argv_ptrs.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > 2) ::close(devnull);
    }
    ::execvp(exe.c_str(), argv_ptrs.data());
    _exit(127);
  }
  int st = 0;
  while (::waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

void test_backlight_dim_restore_and_raii() {
  TempDir tmp("backlight");
  const std::string bl_root = tmp.path + "/backlight";
  const std::string lcd0 = bl_root + "/lcd0";
  assert(cz::mkdir_p(lcd0));

  const std::string brightness_path = lcd0 + "/brightness";
  const std::string max_path = lcd0 + "/max_brightness";
  write_text_file(brightness_path, "128\n");
  write_text_file(max_path, "255\n");

  // Dim, restore, and RAII restore on destruction
  {
    cz::BacklightController bl(bl_root);
    assert(bl.has_hardware_backlight());
    assert(!bl.is_dimmed());

    assert(bl.dim());
    assert(bl.is_dimmed());
    assert(std::stoi(read_text_file(brightness_path)) == 0);

    // Calling dim() again when already dimmed must NOT clobber saved "128"
    assert(bl.dim());
    assert(bl.is_dimmed());

    assert(bl.restore());
    assert(!bl.is_dimmed());
    assert(std::stoi(read_text_file(brightness_path)) == 128);

    // Dim again and let scope exit trigger ~BacklightController() -> restore()
    assert(bl.dim());
    assert(bl.is_dimmed());
    assert(std::stoi(read_text_file(brightness_path)) == 0);
  }
  assert(std::stoi(read_text_file(brightness_path)) == 128);

  // Missing directory safely sets has_hardware_backlight() == false and allows logical dim
  {
    cz::BacklightController missing_bl(tmp.path + "/nonexistent_backlight");
    assert(!missing_bl.has_hardware_backlight());
    assert(!missing_bl.is_dimmed());
    missing_bl.dim();
    assert(missing_bl.is_dimmed());
    missing_bl.restore();
    assert(!missing_bl.is_dimmed());
  }

  // Read-only brightness file safely sets has_hardware_backlight() == false (if not root)
  if (::geteuid() != 0) {
    const std::string ro_root = tmp.path + "/ro_backlight";
    const std::string ro_lcd = ro_root + "/lcd0";
    assert(cz::mkdir_p(ro_lcd));
    const std::string ro_b = ro_lcd + "/brightness";
    write_text_file(ro_b, "200");
    assert(::chmod(ro_b.c_str(), 0444) == 0);

    cz::BacklightController ro_bl(ro_root);
    assert(!ro_bl.has_hardware_backlight());
    ro_bl.dim();
    assert(ro_bl.is_dimmed());
    ro_bl.restore();
    assert(!ro_bl.is_dimmed());
    assert(read_text_file(ro_b) == "200");
    ::chmod(ro_b.c_str(), 0644);
  }
}

void test_timelapse_photos_mode_and_caps() {
  TempDir tmp("tl_photos");
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  cz::SimCameraBackend backend;
  cz::CameraStreamSpec spec;
  spec.mode = "timelapse";
  spec.main_width = 1640;
  spec.main_height = 1232;
  spec.preview_width = 226;
  spec.preview_height = 170;
  spec.fps = 30;
  std::string err;
  assert(backend.start(spec, err));

  cz::AppConfig cfg;
  cfg.output_dir = out_dir;
  cfg.timelapse_resolution = "2mp";
  cfg.timelapse_interval_sec = 1;
  cfg.timelapse_output_mode = "photos";
  cfg.timelapse_stop_mode = "shots";
  cfg.timelapse_stop_value = 3;

  cz::TimelapseEngine engine(&backend);
  uint64_t now_ms = 10000;
  assert(engine.start(cfg, now_ms));
  assert(engine.state().running);

  // Advance virtual clock across 3 intervals
  for (int i = 0; i < 6 && engine.state().running; ++i) {
    engine.tick(now_ms, cfg);
    now_ms += 500;
  }

  const cz::TimelapseState st = engine.state();
  assert(!st.running);
  assert(!st.compiling);
  assert(st.shots_taken == 3);
  assert(st.last_error.empty());
  assert(!st.result_path.empty());
  assert(fs::is_directory(st.result_path));
  assert(fs::path(st.result_path).filename().string().rfind("TL_", 0) == 0);

  // Verify frame_0001.jpg .. frame_0003.jpg exist and no .tmp or .tl_work_* remain
  for (int i = 1; i <= 3; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "frame_%04d.jpg", i);
    const std::string frame_path = st.result_path + "/" + buf;
    assert(fs::exists(frame_path));
    assert(fs::file_size(frame_path) > 0);
    cz::RgbImage img;
    assert(cz::decode_jpeg_rgb24(frame_path, img, err));
    assert(img.width == 1640 && img.height == 1232);
  }
  for (const auto& entry : fs::directory_iterator(st.result_path)) {
    const std::string name = entry.path().filename().string();
    assert(!name.empty() && name[0] != '.');
    assert(name.find(".tmp") == std::string::npos);
  }
  assert(cz::scan_orphaned_timelapses(out_dir).empty());

  // Also test duration cap
  cz::AppConfig dur_cfg = cfg;
  dur_cfg.timelapse_stop_mode = "duration";
  dur_cfg.timelapse_stop_value = 2;  // 2 seconds
  dur_cfg.timelapse_interval_sec = 1;

  uint64_t dur_ms = 50000;
  assert(engine.start(dur_cfg, dur_ms));
  for (int i = 0; i < 10 && engine.state().running; ++i) {
    engine.tick(dur_ms, dur_cfg);
    dur_ms += 500;
  }
  assert(!engine.state().running);
  assert(engine.state().shots_taken >= 2);
  assert(engine.state().elapsed_sec >= 2);

  backend.stop();
}

void test_timelapse_compile_8mp_scaling_and_h264_fallback() {
  TempDir tmp("tl_compile");
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  const cz::TimelapseSessionPaths paths =
      cz::create_timelapse_session(out_dir, true);

  // Command builder verification: 8mp scales to 1440:1080; 2mp and 1080p omit scale
  auto has_arg_pair = [](const std::vector<std::string>& args,
                         const std::string& key,
                         const std::string& val) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (args[i] == key && args[i + 1] == val) return true;
    }
    return false;
  };
  auto has_arg = [](const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
  };

  const auto cmd_8mp_hw =
      cz::TimelapseEngine::build_compile_command(paths, 24, "8mp", true);
  assert(has_arg_pair(cmd_8mp_hw, "-threads", "1"));
  assert(has_arg_pair(cmd_8mp_hw, "-filter_threads", "1"));
  assert(has_arg_pair(cmd_8mp_hw, "-vf", "scale=1024:768:flags=bilinear,setsar=1"));
  assert(has_arg_pair(cmd_8mp_hw, "-c:v", "h264_v4l2m2m"));
  assert(has_arg_pair(cmd_8mp_hw, "-num_output_buffers", "4"));
  assert(has_arg_pair(cmd_8mp_hw, "-b:v", "5M"));
  assert(has_arg_pair(cmd_8mp_hw, "-f", "mp4"));
  assert(has_arg_pair(cmd_8mp_hw, "-movflags", "+faststart"));
  assert(has_arg(cmd_8mp_hw, paths.mp4_tmp_path));

  const auto cmd_2mp_sw =
      cz::TimelapseEngine::build_compile_command(paths, 12, "2mp", false);
  assert(has_arg_pair(cmd_2mp_sw, "-framerate", "12"));
  assert(has_arg_pair(cmd_2mp_sw, "-vf", "scale=1024:768:flags=bilinear,setsar=1"));
  assert(has_arg_pair(cmd_2mp_sw, "-c:v", "libx264"));
  assert(has_arg_pair(cmd_2mp_sw, "-threads", "2"));
  assert(has_arg_pair(cmd_2mp_sw, "-preset", "ultrafast"));
  assert(has_arg_pair(cmd_2mp_sw, "-crf", "23"));
  assert(has_arg_pair(cmd_2mp_sw, "-x264-params",
                      "rc-lookahead=0:ref=1:bframes=0:threads=2:lookahead-threads=1:sync-lookahead=0"));
  assert(has_arg_pair(cmd_2mp_sw, "-f", "mp4"));

  const auto cmd_1080_hw =
      cz::TimelapseEngine::build_compile_command(paths, 30, "1080p", true);
  assert(has_arg_pair(cmd_1080_hw, "-vf", "scale=1280:720:flags=bilinear,setsar=1"));
  assert(has_arg_pair(cmd_1080_hw, "-framerate", "30"));

  // Generate 3 real JPEGs and compile with 8mp scaling (triggers h264_v4l2m2m -> libx264 fallback on macOS)
  for (int i = 1; i <= 3; ++i) {
    const cz::StagedPaths fp = cz::next_timelapse_frame_paths(paths.work_dir, i);
    assert(create_sample_jpeg(fp.final_path, 320, 240, static_cast<uint8_t>(i * 20)));
  }
  cz::TimelapseManifest m;
  m.session_id = paths.session_id;
  m.work_dir = paths.work_dir;
  m.resolution = "8mp";
  m.interval_sec = 5;
  m.fps = 24;
  m.frame_count = 3;
  m.status = "recording";
  assert(cz::write_timelapse_manifest(paths.manifest_path, m));

  cz::TimelapseEngine engine;
  int progress_calls = 0;
  int last_cur = 0;
  int last_tot = 0;
  assert(engine.compile_session(
      paths, 24, "8mp", [&](int cur, int total) {
        ++progress_calls;
        last_cur = cur;
        last_tot = total;
      }));

  assert(progress_calls > 0);
  assert(last_tot == 3);
  assert(last_cur >= 1 && last_cur <= 3);
  assert(!fs::exists(paths.work_dir));
  assert(!fs::exists(paths.mp4_tmp_path));
  assert(fs::exists(paths.mp4_final_path));
  assert(fs::file_size(paths.mp4_final_path) > 0);
  assert(engine.state().result_path == paths.mp4_final_path);
  assert(!engine.state().compiling);
  assert(engine.state().last_error.empty());

  // Probe compiled MP4 via GalleryStore to verify CM0-safe 1024x768 scaled output
  cz::GalleryStore gallery;
  assert(gallery.refresh(out_dir));
  assert(gallery.items().size() == 1);
  cz::GalleryItem item = gallery.items()[0];
  assert(item.type == cz::GalleryItemType::Video);
  assert(gallery.probe_video_meta(item));
  assert(item.width == 1024);
  assert(item.height == 768);

  // Also test video-mode live capture + request_stop() + cancel_compile() preserving staging files
  cz::SimCameraBackend sim;
  cz::CameraStreamSpec spec;
  spec.mode = "timelapse";
  spec.main_width = 1640;
  spec.main_height = 1232;
  spec.preview_width = 226;
  spec.preview_height = 170;
  std::string err;
  assert(sim.start(spec, err));
  engine.set_backend(&sim);

  cz::AppConfig vcfg;
  vcfg.output_dir = out_dir;
  vcfg.timelapse_resolution = "2mp";
  vcfg.timelapse_interval_sec = 1;
  vcfg.timelapse_output_mode = "video";
  vcfg.timelapse_fps = 24;
  vcfg.timelapse_stop_mode = "manual";

  assert(engine.start(vcfg, 1000));
  engine.tick(1000, vcfg);
  engine.tick(2000, vcfg);
  assert(engine.state().shots_taken == 2);
  engine.request_stop();
  assert(!engine.state().running);
  assert(engine.state().needs_compile);
  const cz::TimelapseSessionPaths vpaths = engine.session_paths();
  assert(fs::is_directory(vpaths.work_dir));
  sim.stop();

  // Cancel during compile preserves staging work_dir and leaves no .part or final .mp4
  const cz::TimelapseSessionPaths cancel_paths =
      cz::create_timelapse_session(out_dir, true);
  for (int i = 1; i <= 60; ++i) {
    const cz::StagedPaths fp = cz::next_timelapse_frame_paths(cancel_paths.work_dir, i);
    assert(create_sample_jpeg(fp.final_path, 640, 480, static_cast<uint8_t>(i)));
  }
  std::atomic<bool> cancel_triggered{false};
  bool compile_ok = true;
  std::thread compiler([&]() {
    compile_ok = engine.compile_session(
        cancel_paths, 24, "8mp", [&](int cur, int /*tot*/) {
          if (cur >= 1 && !cancel_triggered.exchange(true)) {
            engine.cancel_compile();
          }
        });
  });
  const auto cancel_dl =
      std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!cancel_triggered.load() &&
         std::chrono::steady_clock::now() < cancel_dl) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!cancel_triggered.load()) {
    engine.cancel_compile();
  }
  compiler.join();
  assert(!compile_ok);
  assert(!engine.state().compiling);
  assert(!engine.state().last_error.empty());
  assert(!fs::exists(cancel_paths.mp4_tmp_path));
  assert(!fs::exists(cancel_paths.mp4_final_path));
  assert(fs::is_directory(cancel_paths.work_dir));
  assert(fs::exists(cancel_paths.work_dir + "/frame_0001.jpg"));
}

void test_timelapse_orphan_recovery_actions() {
  TempDir tmp("tl_recover");
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  auto make_orphan = [&](const std::string& suffix, int frames) {
    const std::string work = out_dir + "/.tl_work_" + suffix;
    assert(cz::mkdir_p(work));
    for (int i = 1; i <= frames; ++i) {
      const cz::StagedPaths fp = cz::next_timelapse_frame_paths(work, i);
      assert(create_sample_jpeg(fp.final_path, 320, 240, static_cast<uint8_t>(i * 10)));
    }
    // Leave a hidden temp file to ensure recovery ignores/cleans it
    write_text_file(work + "/.frame_0099.jpg.tmp", "partial");

    cz::TimelapseManifest m;
    m.session_id = suffix;
    m.work_dir = work;
    m.created_at = "2026-11-19T12:00:00Z";
    m.resolution = "2mp";
    m.interval_sec = 5;
    m.fps = 24;
    m.frame_count = frames;
    m.status = "recording";
    assert(cz::write_timelapse_manifest(work, m));
  };

  make_orphan("20261119_100001", 2);
  make_orphan("20261119_100002", 2);
  make_orphan("20261119_100003", 3);

  const auto orphans = cz::scan_orphaned_timelapses(out_dir);
  assert(orphans.size() == 3);

  cz::TimelapseEngine engine;
  std::string msg;

  // 1. KeepPhotos moves frames to TL_<ts>/ cleanly and removes .tl_work_*
  assert(engine.recover_session(orphans[0], cz::RecoveryAction::KeepPhotos, msg));
  assert(!msg.empty());
  assert(!fs::exists(orphans[0].work_dir));
  const std::string kept_dir = out_dir + "/TL_" + orphans[0].session_id;
  assert(fs::is_directory(kept_dir));
  assert(fs::exists(kept_dir + "/frame_0001.jpg"));
  assert(fs::exists(kept_dir + "/frame_0002.jpg"));
  assert(!fs::exists(kept_dir + "/session.json"));
  assert(!fs::exists(kept_dir + "/.frame_0099.jpg.tmp"));

  // 2. Discard deletes .tl_work_* cleanly
  assert(engine.recover_session(orphans[1], cz::RecoveryAction::Discard, msg));
  assert(!msg.empty());
  assert(!fs::exists(orphans[1].work_dir));
  assert(!fs::exists(out_dir + "/TL_" + orphans[1].session_id));
  assert(!fs::exists(out_dir + "/TL_" + orphans[1].session_id + ".mp4"));

  // 3. Compile compiles to TL_<ts>.mp4 and deletes .tl_work_*
  assert(engine.recover_session(orphans[2], cz::RecoveryAction::Compile, msg));
  assert(!msg.empty());
  assert(!fs::exists(orphans[2].work_dir));
  const std::string compiled_mp4 = out_dir + "/TL_" + orphans[2].session_id + ".mp4";
  assert(fs::exists(compiled_mp4));
  assert(fs::file_size(compiled_mp4) > 0);
  assert(cz::scan_orphaned_timelapses(out_dir).empty());
}

void test_gallery_scan_ignores_hidden_and_deletes_safely() {
  TempDir tmp("gallery");
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  // Create visible entries + hidden entries
  const std::string img_path = out_dir + "/IMG_1.jpg";
  assert(create_sample_jpeg(img_path, 640, 480, 11));

  const std::string vid_path = out_dir + "/VID_1.mp4";
  assert(run_ffmpeg_sync({"-y", "-f", "lavfi", "-i",
                          "testsrc=duration=1:size=320x240:rate=15",
                          "-c:v", "libx264", "-pix_fmt", "yuv420p", vid_path}));

  const std::string tl_dir = out_dir + "/TL_20261119_1200";
  assert(cz::mkdir_p(tl_dir));
  assert(create_sample_jpeg(tl_dir + "/frame_0001.jpg", 640, 480, 33));
  assert(create_sample_jpeg(tl_dir + "/frame_0002.jpg", 640, 480, 44));

  // Hidden staging files/dirs that MUST be ignored
  write_text_file(out_dir + "/.IMG_2.jpg.tmp", "hidden_jpg");
  write_text_file(out_dir + "/.VID_2.mp4.part", "hidden_mp4");
  assert(cz::mkdir_p(out_dir + "/.tl_work_20261119_1100"));
  assert(create_sample_jpeg(out_dir + "/.tl_work_20261119_1100/frame_0001.jpg",
                            320, 240, 55));

  // Set deterministic mtimes so newest-first order is strictly:
  // 0: TL_20261119_1200 (newest)
  // 1: VID_1.mp4        (middle)
  // 2: IMG_1.jpg        (oldest)
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(img_path, now - std::chrono::seconds(30));
  fs::last_write_time(vid_path, now - std::chrono::seconds(15));
  fs::last_write_time(tl_dir, now - std::chrono::seconds(5));

  cz::GalleryStore store;
  assert(store.refresh(out_dir));
  assert(!store.in_session());
  assert(store.items().size() == 3);

  assert(store.items()[0].filename == "TL_20261119_1200");
  assert(store.items()[0].type == cz::GalleryItemType::TimelapseFolder);
  assert(store.items()[0].photo_count == 2);

  assert(store.items()[1].filename == "VID_1.mp4");
  assert(store.items()[1].type == cz::GalleryItemType::Video);

  assert(store.items()[2].filename == "IMG_1.jpg");
  assert(store.items()[2].type == cz::GalleryItemType::Photo);

  // Probe video metadata
  cz::GalleryItem vid_meta = store.items()[1];
  assert(store.probe_video_meta(vid_meta));
  assert(vid_meta.width == 320 && vid_meta.height == 240);
  assert(vid_meta.duration_sec > 0.5);

  // Load previews for Photo, Video poster, and TimelapseFolder
  std::string err;
  cz::RgbImage preview;
  assert(store.load_preview_rgb24(2, 320, 128, preview, err));
  assert(preview.width > 0 && preview.width <= 320);
  assert(preview.height > 0 && preview.height <= 128);
  assert(preview.rgb24.size() ==
         static_cast<size_t>(preview.width) * preview.height * 3u);

  assert(store.load_preview_rgb24(1, 320, 128, preview, err));
  assert(preview.width > 0 && preview.width <= 320);
  assert(preview.height > 0 && preview.height <= 128);

  assert(store.load_preview_rgb24(0, 320, 128, preview, err));
  assert(preview.width > 0 && preview.width <= 320);
  assert(preview.height > 0 && preview.height <= 128);

  // Enter and exit TL session
  assert(store.enter_session(0));
  assert(store.in_session());
  assert(store.session_name() == "TL_20261119_1200");
  assert(store.items().size() == 2);
  assert(store.items()[0].filename == "frame_0001.jpg");
  assert(store.items()[1].filename == "frame_0002.jpg");

  // Delete single photo inside session
  assert(store.delete_item(0, err));
  assert(store.items().size() == 1);
  assert(!fs::exists(tl_dir + "/frame_0001.jpg"));
  assert(fs::exists(tl_dir + "/frame_0002.jpg"));

  // Exit session back to root
  assert(store.exit_session());
  assert(!store.in_session());
  assert(store.items().size() == 3);
  assert(store.items()[0].photo_count == 1);

  // Delete TL folder, Video, and Photo from root
  assert(store.delete_item(0, err));
  assert(!fs::exists(tl_dir));
  assert(store.items().size() == 2);

  assert(store.delete_item(0, err));
  assert(!fs::exists(vid_path));
  assert(store.items().size() == 1);

  assert(store.delete_item(0, err));
  assert(!fs::exists(img_path));
  assert(store.items().empty());

  // Out-of-bounds and missing directory safety
  assert(!store.enter_session(0));
  assert(!store.exit_session());
  assert(!store.delete_item(-1, err));
  assert(!store.load_preview_rgb24(0, 320, 128, preview, err));
  assert(!store.refresh(tmp.path + "/does_not_exist"));
}

void test_video_player_decodes_frames_seeks_and_handles_silent_mp4() {
  TempDir tmp("vplayer");
  const std::string mp4_path = tmp.path + "/silent_test.mp4";
  assert(run_ffmpeg_sync({"-y", "-f", "lavfi", "-i",
                          "testsrc=duration=2:size=320x240:rate=15",
                          "-c:v", "libx264", "-pix_fmt", "yuv420p", mp4_path}));

  cz::VideoPlayer player;
  assert(player.open(mp4_path, 226, 128));
  const cz::VideoPlayerState opened = player.state();
  assert(opened.stopped);
  assert(!opened.playing);
  assert(!opened.paused);
  assert(opened.duration_sec > 1.5);
  assert(opened.width > 0 && opened.width <= 226);
  assert(opened.height > 0 && opened.height <= 128);
  assert(opened.error.empty());

  // play(true) MUST succeed cleanly on a silent MP4 lacking an audio stream
  assert(player.play(true));
  assert(player.state().playing);
  assert(!player.state().paused);
  assert(player.state().error.empty());

  cz::PreviewFrame f1;
  bool got_f1 = false;
  const auto dl1 = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < dl1) {
    if (player.poll_frame(f1)) {
      got_f1 = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  assert(got_f1);
  assert(f1.width > 0 && f1.width <= 226);
  assert(f1.height > 0 && f1.height <= 128);
  assert(f1.rgb24.size() ==
         static_cast<size_t>(f1.width) * static_cast<size_t>(f1.height) * 3u);

  // Seek forward +1.0s and receive a newer frame
  assert(player.seek_delta(1.0));
  cz::PreviewFrame f_seek;
  bool got_seek = false;
  const auto dl2 = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < dl2) {
    if (player.poll_frame(f_seek)) {
      got_seek = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  assert(got_seek);
  assert(f_seek.seq > f1.seq);
  assert(player.state().position_sec >= 0.9);

  player.toggle_mute();
  assert(player.state().muted);
  player.toggle_mute();
  assert(!player.state().muted);

  player.pause();
  assert(player.state().paused);
  assert(!player.state().playing);

  player.stop();
  assert(player.state().stopped);
  assert(!player.state().playing);
  assert(!player.state().paused);

  // Also test MP4 with audio + pcm_sink callback
  const std::string audio_mp4 = tmp.path + "/audio_test.mp4";
  assert(run_ffmpeg_sync({"-y", "-f", "lavfi", "-i",
                          "testsrc=duration=1:size=320x240:rate=15",
                          "-f", "lavfi", "-i",
                          "sine=frequency=440:sample_rate=44100:duration=1",
                          "-shortest", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                          "-c:a", "aac", "-b:a", "64k", audio_mp4}));

  std::atomic<size_t> pcm_samples{0};
  player.set_pcm_sink([&](const int16_t* data, size_t count) {
    if (data && count > 0) {
      pcm_samples.fetch_add(count);
    }
  });
  assert(player.open(audio_mp4, 226, 128));
  assert(player.play(true));
  cz::PreviewFrame af;
  const auto dl3 = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (std::chrono::steady_clock::now() < dl3) {
    player.poll_frame(af);
    if (af.seq > 0 && pcm_samples.load() > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(af.seq > 0);
  assert(pcm_samples.load() > 0);
  player.stop();

  // Muted play(false) on audio MP4 skips pcm_sink and reaches EOF naturally
  pcm_samples.store(0);
  assert(player.open(audio_mp4, 226, 128));
  assert(player.state().has_audio);
  assert(player.play(false));
  assert(player.state().muted);
  assert(player.seek_delta(-10.0));
  assert(player.state().position_sec >= 0.0 && player.state().position_sec < 0.1);
  const auto dl4 = std::chrono::steady_clock::now() + std::chrono::seconds(4);
  while (!player.state().eof && std::chrono::steady_clock::now() < dl4) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  assert(player.state().eof);
  assert(player.state().stopped);
  assert(pcm_samples.load() == 0);
  player.stop();

  // Missing video file fails cleanly
  assert(!player.open(tmp.path + "/nonexistent.mp4", 226, 128));
  assert(!player.state().error.empty());
}

}  // namespace

int main() {
  test_backlight_dim_restore_and_raii();
  test_timelapse_photos_mode_and_caps();
  test_timelapse_compile_8mp_scaling_and_h264_fallback();
  test_timelapse_orphan_recovery_actions();
  test_gallery_scan_ignores_hidden_and_deletes_safely();
  test_video_player_decodes_frames_seeks_and_handles_silent_mp4();
  std::cout << "All test_timelapse_and_gallery tests passed!\n";
  return 0;
}
