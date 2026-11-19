#include "config.hpp"
#include "fb_blit.hpp"
#include "storage.hpp"
#include "theme.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TempDir {
  std::string path;
  explicit TempDir(const std::string& name) {
    const std::string base = "/tmp/czcamera_test_" + name + "_" + std::to_string(::getpid());
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

void write_bytes(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  assert(out.is_open());
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  assert(out.good());
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  assert(in.is_open());
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

void test_theme_tokens_and_toggle() {
  const cz::ShadcnPalette dark = cz::get_palette(cz::ThemeMode::Dark);
  assert(dark.background == 0x09090b);
  assert(dark.card == 0x09090b);
  assert(dark.muted == 0x18181b);
  assert(dark.border == 0x27272a);
  assert(dark.ring == 0xd4d4d8);
  assert(dark.foreground == 0xfafafa);
  assert(dark.muted_foreground == 0xa1a1aa);
  assert(dark.status_online == 0x4ade80);
  assert(dark.status_sleeping == 0xfbbf24);
  assert(dark.status_error == 0xf87171);
  assert(dark.viewfinder_surround == 0x050507);

  const cz::ShadcnPalette light = cz::get_palette(cz::ThemeMode::Light);
  assert(light.background == 0xfafafa);
  assert(light.card == 0xffffff);
  assert(light.muted == 0xf4f4f5);
  assert(light.border == 0xe4e4e7);
  assert(light.ring == 0x71717a);
  assert(light.foreground == 0x09090b);
  assert(light.muted_foreground == 0x71717a);
  assert(light.status_online == 0x16a34a);
  assert(light.status_sleeping == 0xd97706);
  assert(light.status_error == 0xdc2626);
  assert(light.viewfinder_surround == 0x050507);

  assert(cz::toggle_theme(cz::ThemeMode::Dark) == cz::ThemeMode::Light);
  assert(cz::toggle_theme(cz::ThemeMode::Light) == cz::ThemeMode::Dark);
  assert(cz::theme_to_string(cz::ThemeMode::Dark) == "dark");
  assert(cz::theme_to_string(cz::ThemeMode::Light) == "light");
  assert(cz::theme_from_string("light") == cz::ThemeMode::Light);
  assert(cz::theme_from_string("dark") == cz::ThemeMode::Dark);
  assert(cz::theme_from_string("invalid") == cz::ThemeMode::Dark);
}

void test_fb_blit_rgb565_argb32_and_bounds() {
  // Channel packing checks
  const cz::FbChannel r5{11, 5};
  const cz::FbChannel g6{5, 6};
  const cz::FbChannel b5{0, 5};
  assert(cz::pack_fb_channel(0xFF, r5) == 0xF800u);
  assert(cz::pack_fb_channel(0xFF, g6) == 0x07E0u);
  assert(cz::pack_fb_channel(0xFF, b5) == 0x001Fu);
  assert(cz::pack_fb_channel(0x00, r5) == 0x0000u);
  assert(cz::pack_fb_channel(0xFF, cz::FbChannel{0, 0}) == 0u);
  assert(cz::pack_fb_channel(0xFF, cz::FbChannel{32, 8}) == 0u);

  // RGB565 blit with xoffset, yoffset, and padded line_length
  {
    cz::FbBlitSpec spec{};
    spec.xres = 2;
    spec.yres = 2;
    spec.xres_virtual = 4;
    spec.yres_virtual = 4;
    spec.xoffset = 1;
    spec.yoffset = 1;
    spec.bits_per_pixel = 16;
    spec.line_length = 12;  // padded stride > 4*2
    spec.red = {11, 5};
    spec.green = {5, 6};
    spec.blue = {0, 5};

    std::vector<uint8_t> fb(spec.yres_virtual * spec.line_length, 0x00);
    const uint32_t src[4] = {
        0xFF0000, 0x00FF00,
        0x0000FF, 0xFFFFFF,
    };

    assert(cz::blit_framebuffer(fb.data(), fb.size(), spec, src, 2, 2));

    auto read16 = [&](uint32_t row, uint32_t col) -> uint16_t {
      const size_t off = static_cast<size_t>(row) * spec.line_length +
                         static_cast<size_t>(col) * 2u;
      return static_cast<uint16_t>(fb[off]) |
             (static_cast<uint16_t>(fb[off + 1]) << 8);
    };
    assert(read16(1, 1) == 0xF800u);
    assert(read16(1, 2) == 0x07E0u);
    assert(read16(2, 1) == 0x001Fu);
    assert(read16(2, 2) == 0xFFFFu);
    assert(read16(0, 0) == 0x0000u);
  }

  // XRGB8888 blit check
  {
    cz::FbBlitSpec spec{};
    spec.xres = 2;
    spec.yres = 1;
    spec.xres_virtual = 2;
    spec.yres_virtual = 1;
    spec.bits_per_pixel = 32;
    spec.line_length = 8;
    spec.red = {16, 8};
    spec.green = {8, 8};
    spec.blue = {0, 8};
    spec.transp = {24, 8};

    std::vector<uint8_t> fb(8, 0);
    const uint32_t src[2] = {0x123456, 0xABCDEF};
    assert(cz::blit_framebuffer(fb.data(), fb.size(), spec, src, 2, 1));

    const uint32_t p0 = static_cast<uint32_t>(fb[0]) |
                        (static_cast<uint32_t>(fb[1]) << 8) |
                        (static_cast<uint32_t>(fb[2]) << 16) |
                        (static_cast<uint32_t>(fb[3]) << 24);
    const uint32_t p1 = static_cast<uint32_t>(fb[4]) |
                        (static_cast<uint32_t>(fb[5]) << 8) |
                        (static_cast<uint32_t>(fb[6]) << 16) |
                        (static_cast<uint32_t>(fb[7]) << 24);
    assert(p0 == 0xFF123456u);
    assert(p1 == 0xFFABCDEFu);
  }

  // 24bpp RGB24 blit check
  {
    cz::FbBlitSpec spec{};
    spec.xres = 2;
    spec.yres = 1;
    spec.xres_virtual = 2;
    spec.yres_virtual = 1;
    spec.bits_per_pixel = 24;
    spec.line_length = 6;
    spec.red = {16, 8};
    spec.green = {8, 8};
    spec.blue = {0, 8};

    std::vector<uint8_t> fb(6, 0);
    const uint32_t src[2] = {0x112233, 0xAABBCC};
    assert(cz::blit_framebuffer(fb.data(), fb.size(), spec, src, 2, 1));
    assert(fb[0] == 0x33 && fb[1] == 0x22 && fb[2] == 0x11);
    assert(fb[3] == 0xCC && fb[4] == 0xBB && fb[5] == 0xAA);
  }

  // Bounds / error rejections
  {
    cz::FbBlitSpec spec{};
    spec.xres = 4;
    spec.yres = 4;
    spec.xres_virtual = 4;
    spec.yres_virtual = 4;
    spec.bits_per_pixel = 16;
    spec.line_length = 8;
    spec.red = {11, 5};
    spec.green = {5, 6};
    spec.blue = {0, 5};

    std::vector<uint8_t> fb(32, 0);
    std::vector<uint32_t> src(16, 0x112233);

    // Undersized map_size
    assert(!cz::blit_framebuffer(fb.data(), 16, spec, src.data(), 4, 4));

    // xoffset exceeding virtual width
    cz::FbBlitSpec bad_xoff = spec;
    bad_xoff.xoffset = 1;
    assert(!cz::blit_framebuffer(fb.data(), fb.size(), bad_xoff, src.data(), 4, 4));

    // yoffset exceeding virtual height
    cz::FbBlitSpec bad_yoff = spec;
    bad_yoff.yoffset = 1;
    assert(!cz::blit_framebuffer(fb.data(), fb.size(), bad_yoff, src.data(), 4, 4));

    // Bitfield offset exceeding bits_per_pixel
    cz::FbBlitSpec bad_bf = spec;
    bad_bf.red = {16, 8};
    assert(!cz::blit_framebuffer(fb.data(), fb.size(), bad_bf, src.data(), 4, 4));

    // Null pointers / unsupported bpp
    assert(!cz::blit_framebuffer(nullptr, fb.size(), spec, src.data(), 4, 4));
    assert(!cz::blit_framebuffer(fb.data(), fb.size(), spec, nullptr, 4, 4));
    cz::FbBlitSpec bad_bpp = spec;
    bad_bpp.bits_per_pixel = 8;
    assert(!cz::blit_framebuffer(fb.data(), fb.size(), bad_bpp, src.data(), 4, 4));
  }
}

void test_config_defaults_roundtrip_and_corrupt_fallback() {
  TempDir tmp("config");
  const std::string cfg_path = tmp.path + "/nested/config.json";

  // Missing config returns all 18 defaults
  const cz::AppConfig def = cz::load_config(cfg_path);
  assert(def.output_dir == "~/Pictures/czcamera");
  assert(def.theme == "dark");
  assert(def.rail_side == "right");
  assert(def.photo_resolution == "8mp");
  assert(def.photo_filter == "none");
  assert(def.photo_timer_sec == 0);
  assert(def.grid_enabled == true);
  assert(def.video_resolution == "720p");
  assert(def.video_audio == true);
  assert(def.timelapse_resolution == "2mp");
  assert(def.timelapse_interval_sec == 5);
  assert(def.timelapse_output_mode == "video");
  assert(def.timelapse_fps == 24);
  assert(def.timelapse_stop_mode == "manual");
  assert(def.timelapse_stop_value == 0);
  assert(def.timelapse_dim_sec == 15);
  assert(def.ev == 0.0);
  assert(def.camera_transform == "auto");

  // CZCAMERA_CONFIG override
  const std::string env_cfg = tmp.path + "/env_config.json";
  ::setenv("CZCAMERA_CONFIG", env_cfg.c_str(), 1);
  assert(cz::default_config_path() == env_cfg);

  // expand_user_path check
  const char* home = std::getenv("HOME");
  if (home && *home) {
    assert(cz::expand_user_path("~/Pictures/czcamera") ==
           std::string(home) + "/Pictures/czcamera");
    assert(cz::expand_user_path("~") == std::string(home));
  }
  assert(cz::expand_user_path("/tmp/abc") == "/tmp/abc");

  // Save/load round-trip of all 18 fields with non-default values
  cz::AppConfig custom;
  custom.output_dir = "/tmp/my_cam_dir";
  custom.theme = "light";
  custom.rail_side = "left";
  custom.photo_resolution = "2mp";
  custom.photo_filter = "gameboy";
  custom.photo_timer_sec = 10;
  custom.grid_enabled = false;
  custom.video_resolution = "1080p";
  custom.video_audio = false;
  custom.timelapse_resolution = "8mp";
  custom.timelapse_interval_sec = 30;
  custom.timelapse_output_mode = "photos";
  custom.timelapse_fps = 30;
  custom.timelapse_stop_mode = "shots";
  custom.timelapse_stop_value = 120;
  custom.timelapse_dim_sec = 60;
  custom.ev = -1.5;
  custom.camera_transform = "180";

  assert(cz::save_config(custom));
  const cz::AppConfig loaded = cz::load_config();
  assert(loaded.output_dir == custom.output_dir);
  assert(loaded.theme == custom.theme);
  assert(loaded.rail_side == custom.rail_side);
  assert(loaded.photo_resolution == custom.photo_resolution);
  assert(loaded.photo_filter == custom.photo_filter);
  assert(loaded.photo_timer_sec == custom.photo_timer_sec);
  assert(loaded.grid_enabled == custom.grid_enabled);
  assert(loaded.video_resolution == custom.video_resolution);
  assert(loaded.video_audio == custom.video_audio);
  assert(loaded.timelapse_resolution == custom.timelapse_resolution);
  assert(loaded.timelapse_interval_sec == custom.timelapse_interval_sec);
  assert(loaded.timelapse_output_mode == custom.timelapse_output_mode);
  assert(loaded.timelapse_fps == custom.timelapse_fps);
  assert(loaded.timelapse_stop_mode == custom.timelapse_stop_mode);
  assert(loaded.timelapse_stop_value == custom.timelapse_stop_value);
  assert(loaded.timelapse_dim_sec == custom.timelapse_dim_sec);
  assert(loaded.ev == custom.ev);
  assert(loaded.camera_transform == custom.camera_transform);

  // Corrupt JSON falls back gracefully to defaults without throwing
  write_bytes(env_cfg, "{ \"theme\": \"light\", broken_json ");
  const cz::AppConfig recovered = cz::load_config();
  assert(recovered.theme == "dark");
  assert(recovered.output_dir == "~/Pictures/czcamera");
  assert(recovered.timelapse_dim_sec == 15);
  assert(recovered.camera_transform == "auto");

  ::unsetenv("CZCAMERA_CONFIG");
}

void test_sync_safe_staging_collision_storage_floor_and_manifest() {
  TempDir tmp("storage");
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  // Photo staging paths & same-second collision
  const cz::StagedPaths p1 = cz::next_photo_paths(out_dir);
  const std::string p1_tmp_name = fs::path(p1.tmp_path).filename().string();
  const std::string p1_fin_name = fs::path(p1.final_path).filename().string();
  assert(fs::path(p1.tmp_path).parent_path() == out_dir);
  assert(fs::path(p1.final_path).parent_path() == out_dir);
  assert(p1_tmp_name.rfind(".IMG_", 0) == 0);
  assert(p1_tmp_name.find(".jpg.tmp") != std::string::npos);
  assert(p1_fin_name.rfind("IMG_", 0) == 0);
  assert(p1_fin_name.find(".jpg") != std::string::npos);

  // Create p1.final_path so next_photo_paths in the same second collides
  write_bytes(p1.final_path, "jpegdata");
  cz::StagedPaths p2 = cz::next_photo_paths(out_dir);
  if (fs::path(p2.final_path).filename().string().substr(0, 19) !=
      p1_fin_name.substr(0, 19)) {
    // Second rolled over between p1 and p2; collide on p2's second instead.
    write_bytes(p2.final_path, "jpegdata");
    p2 = cz::next_photo_paths(out_dir);
  }
  assert(p2.final_path.find("_01.jpg") != std::string::npos);
  assert(p2.tmp_path.find("_01.jpg.tmp") != std::string::npos);
  assert(p2.final_path != p1.final_path);

  // Video staging paths & collision
  const cz::StagedPaths v1 = cz::next_video_paths(out_dir);
  const std::string v1_tmp_name = fs::path(v1.tmp_path).filename().string();
  const std::string v1_fin_name = fs::path(v1.final_path).filename().string();
  assert(v1_tmp_name.rfind(".VID_", 0) == 0);
  assert(v1_tmp_name.find(".mp4.part") != std::string::npos);
  assert(v1_fin_name.rfind("VID_", 0) == 0);
  assert(v1_fin_name.find(".mp4") != std::string::npos);

  write_bytes(v1.final_path, "mp4data");
  cz::StagedPaths v2 = cz::next_video_paths(out_dir);
  if (fs::path(v2.final_path).filename().string().substr(0, 19) !=
      v1_fin_name.substr(0, 19)) {
    write_bytes(v2.final_path, "mp4data");
    v2 = cz::next_video_paths(out_dir);
  }
  assert(v2.final_path.find("_01.mp4") != std::string::npos);
  assert(v2.tmp_path.find("_01.mp4.part") != std::string::npos);
  assert(v2.final_path != v1.final_path);

  // commit_staged_file rejects missing and 0-byte tmp files without leaking
  std::string err;
  const std::string missing_tmp = out_dir + "/.missing.jpg.tmp";
  const std::string missing_final = out_dir + "/missing.jpg";
  assert(!cz::commit_staged_file(missing_tmp, missing_final, err));
  assert(!err.empty());
  assert(!fs::exists(missing_final));

  const std::string empty_tmp = out_dir + "/.empty.jpg.tmp";
  const std::string empty_final = out_dir + "/empty.jpg";
  write_bytes(empty_tmp, "");
  assert(!cz::commit_staged_file(empty_tmp, empty_final, err));
  assert(!err.empty());
  assert(!fs::exists(empty_final));

  // Valid non-empty tmp commits cleanly
  const std::string good_tmp = out_dir + "/.good.jpg.tmp";
  const std::string good_final = out_dir + "/good.jpg";
  write_bytes(good_tmp, "valid_jpeg_bytes");
  assert(cz::commit_staged_file(good_tmp, good_final, err));
  assert(!fs::exists(good_tmp));
  assert(fs::exists(good_final));
  assert(read_text(good_final) == "valid_jpeg_bytes");

  // Storage floor check (104857600ULL default)
  uint64_t free_bytes = 0;
  const bool ok_default = cz::has_sufficient_storage(out_dir, 104857600ULL, &free_bytes);
  assert(free_bytes > 0);
  assert(ok_default == (free_bytes >= 104857600ULL));
  // Impossibly high threshold must return false
  assert(!cz::has_sufficient_storage(out_dir, UINT64_MAX, &free_bytes));

  // Timelapse session, manifest I/O, and orphan scanning
  const cz::TimelapseSessionPaths sess = cz::create_timelapse_session(out_dir);
  assert(fs::is_directory(sess.work_dir));
  assert(fs::path(sess.work_dir).filename().string().rfind(".tl_work_", 0) == 0);

  cz::TimelapseManifest m;
  m.session_id = sess.session_id;
  m.work_dir = sess.work_dir;
  m.created_at = "2026-11-19T12:30:00Z";
  m.resolution = "2mp";
  m.interval_sec = 5;
  m.fps = 24;
  m.frame_count = 2;
  m.status = "recording";
  assert(cz::write_timelapse_manifest(sess.manifest_path, m));

  cz::TimelapseManifest m_read;
  assert(cz::read_timelapse_manifest(sess.manifest_path, m_read));
  assert(m_read.session_id == m.session_id);
  assert(m_read.created_at == m.created_at);
  assert(m_read.resolution == "2mp");
  assert(m_read.interval_sec == 5);
  assert(m_read.fps == 24);
  assert(m_read.frame_count == 2);
  assert(m_read.status == "recording");

  // Add 2 valid frames to sess.work_dir and create an empty .tl_work_ dir
  write_bytes(sess.work_dir + "/frame_0001.jpg", "jpg1");
  write_bytes(sess.work_dir + "/frame_0002.jpg", "jpg2");

  const std::string empty_work = out_dir + "/.tl_work_20260101_000000";
  assert(cz::mkdir_p(empty_work));
  cz::TimelapseManifest empty_m = m;
  empty_m.session_id = "20260101_000000";
  empty_m.work_dir = empty_work;
  empty_m.frame_count = 0;
  assert(cz::write_timelapse_manifest(empty_work, empty_m));

  const std::vector<cz::TimelapseManifest> orphans = cz::scan_orphaned_timelapses(out_dir);
  assert(orphans.size() == 1);
  assert(orphans[0].session_id == sess.session_id);
  assert(orphans[0].work_dir == sess.work_dir);
  assert(orphans[0].frame_count == 2);
}

}  // namespace

int main() {
  test_theme_tokens_and_toggle();
  test_fb_blit_rgb565_argb32_and_bounds();
  test_config_defaults_roundtrip_and_corrupt_fallback();
  test_sync_safe_staging_collision_storage_floor_and_manifest();
  std::cout << "test_config_and_storage: all tests passed\n";
  return 0;
}
