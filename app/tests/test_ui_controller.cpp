#include "app_ui.hpp"
#include "config.hpp"
#include "evdev_repeat_guard.hpp"
#include "sim_backend.hpp"
#include "storage.hpp"
#include "ui_controller.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TempDir {
  std::string path;
  explicit TempDir(const std::string& name) {
    const std::string base =
        "/tmp/czcamera_test_t6_" + name + "_" + std::to_string(::getpid());
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

void write_text(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  assert(out.is_open());
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.close();
  assert(out.good());
}

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  assert(in.is_open());
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

int count_files_with_ext(const std::string& dir, const std::string& ext) {
  int count = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    if (name.size() >= ext.size() &&
        name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
      ++count;
    }
  }
  return count;
}

int count_hidden_files(const std::string& dir) {
  int count = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (!name.empty() && name[0] == '.') {
      ++count;
    }
  }
  return count;
}

class SpyCameraBackend final : public cz::ICameraBackend {
 public:
  bool start(const cz::CameraStreamSpec& initial_spec,
             std::string& out_err) override {
    start_specs.push_back(initial_spec);
    last_zoom = initial_spec.zoom;
    last_ev = initial_spec.ev;
    return inner_.start(initial_spec, out_err);
  }

  void stop() override { inner_.stop(); }

  bool reconfigure(const cz::CameraStreamSpec& spec,
                   std::string& out_err) override {
    reconfigure_specs.push_back(spec);
    last_zoom = spec.zoom;
    last_ev = spec.ev;
    return inner_.reconfigure(spec, out_err);
  }

  bool set_controls(float zoom, float ev, std::string& out_err) override {
    last_zoom = zoom;
    last_ev = ev;
    ++set_controls_calls;
    return inner_.set_controls(zoom, ev, out_err);
  }

  bool pause(std::string& out_err) override {
    ++pause_calls;
    return inner_.pause(out_err);
  }

  bool resume(std::string& out_err) override {
    ++resume_calls;
    return inner_.resume(out_err);
  }

  bool copy_latest_preview(uint64_t last_seen_seq,
                           cz::PreviewFrame& out_frame) override {
    return inner_.copy_latest_preview(last_seen_seq, out_frame);
  }

  cz::StillCaptureResult capture_still(const std::string& tmp_jpeg_path,
                                       int width,
                                       int height) override {
    ++capture_still_calls;
    return inner_.capture_still(tmp_jpeg_path, width, height);
  }

  bool start_video(const cz::VideoSpec& spec, std::string& out_err) override {
    return inner_.start_video(spec, out_err);
  }

  cz::VideoStopResult stop_video() override { return inner_.stop_video(); }

  cz::BackendHealth status() const override { return inner_.status(); }

  std::vector<cz::CameraStreamSpec> start_specs;
  std::vector<cz::CameraStreamSpec> reconfigure_specs;
  float last_zoom = 1.0f;
  float last_ev = 0.0f;
  int set_controls_calls = 0;
  int pause_calls = 0;
  int resume_calls = 0;
  int capture_still_calls = 0;

 private:
  cz::SimCameraBackend inner_;
};

class DropSimBackend final : public cz::ICameraBackend {
 public:
  bool start(const cz::CameraStreamSpec& initial_spec,
             std::string& out_err) override {
    if (fail_start) {
      out_err = "Simulated camera start failure";
      return false;
    }
    force_offline = false;
    forced_error.clear();
    return inner_.start(initial_spec, out_err);
  }

  void stop() override { inner_.stop(); }

  bool reconfigure(const cz::CameraStreamSpec& spec,
                   std::string& out_err) override {
    return inner_.reconfigure(spec, out_err);
  }

  bool set_controls(float zoom, float ev, std::string& out_err) override {
    return inner_.set_controls(zoom, ev, out_err);
  }

  bool pause(std::string& out_err) override { return inner_.pause(out_err); }

  bool resume(std::string& out_err) override { return inner_.resume(out_err); }

  bool copy_latest_preview(uint64_t last_seen_seq,
                           cz::PreviewFrame& out_frame) override {
    return inner_.copy_latest_preview(last_seen_seq, out_frame);
  }

  cz::StillCaptureResult capture_still(const std::string& tmp_jpeg_path,
                                       int width,
                                       int height) override {
    return inner_.capture_still(tmp_jpeg_path, width, height);
  }

  bool start_video(const cz::VideoSpec& spec, std::string& out_err) override {
    return inner_.start_video(spec, out_err);
  }

  cz::VideoStopResult stop_video() override { return inner_.stop_video(); }

  cz::BackendHealth status() const override {
    cz::BackendHealth h = inner_.status();
    if (force_offline) {
      h.ready = false;
      h.recording = false;
      h.last_error = forced_error;
    }
    return h;
  }

  void simulate_drop(const std::string& err) {
    force_offline = true;
    forced_error = err;
  }

  bool fail_start = false;
  bool force_offline = false;
  std::string forced_error;

 private:
  cz::SimCameraBackend inner_;
};

void test_photo_controls_zoom_loupe_filters_and_timer() {
  TempDir tmp("photo");
  const std::string cfg_path = tmp.path + "/config.json";
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  cz::AppConfig cfg;
  cfg.output_dir = out_dir;
  assert(cz::save_config(cfg, cfg_path));

  auto spy_owned = std::make_unique<SpyCameraBackend>();
  SpyCameraBackend* spy = spy_owned.get();

  cz::UiControllerOptions opts;
  opts.config_path = cfg_path;
  opts.backlight_sysfs_root = tmp.path + "/no_backlight";

  cz::UiController ctrl(std::move(spy_owned), opts);
  uint64_t now = 1000;
  assert(ctrl.init(now));
  assert(ctrl.state().mode == cz::AppMode::Photo);
  assert(ctrl.state().zoom == 1.0f);
  assert(!ctrl.state().loupe_enabled);
  assert(ctrl.state().filter == cz::FilterId::None);
  assert(ctrl.state().config.photo_resolution == "8mp");
  assert(ctrl.state().config.photo_timer_sec == 0);

  // Crop zoom 5/7 steps through 1.0 -> 1.5 -> 2.0 -> 3.0 -> 4.0 and back
  ctrl.handle_key('7', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 1.5f);
  assert(spy->last_zoom == 1.5f);
  ctrl.handle_key('7', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 2.0f);
  ctrl.handle_key('7', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 3.0f);
  ctrl.handle_key('7', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 4.0f);
  ctrl.handle_key('7', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 4.0f);

  ctrl.handle_key('5', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 3.0f);
  ctrl.handle_key('5', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 2.0f);
  ctrl.handle_key('5', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 1.5f);
  ctrl.handle_key('5', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 1.0f);
  ctrl.handle_key('5', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 1.0f);

  // Loupe L toggles 2x viewfinder magnification without changing sensor zoom
  ctrl.handle_key('7', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 1.5f);
  const int calls_before_loupe = spy->set_controls_calls;
  ctrl.handle_key('l', cz::KeyCode::Char, now);
  assert(ctrl.state().loupe_enabled);
  assert(ctrl.state().zoom == 1.5f);
  assert(spy->last_zoom == 1.5f);
  assert(spy->set_controls_calls == calls_before_loupe);
  ctrl.handle_key('L', cz::KeyCode::Char, now);
  assert(!ctrl.state().loupe_enabled);

  // F cycles through all 9 filters and wraps back to None
  const cz::FilterId expected_filters[9] = {
      cz::FilterId::BW,    cz::FilterId::HiCon,  cz::FilterId::Sepia,
      cz::FilterId::Cool,  cz::FilterId::Vivid,  cz::FilterId::Invert,
      cz::FilterId::Retro, cz::FilterId::GameBoy, cz::FilterId::None,
  };
  for (cz::FilterId ef : expected_filters) {
    ctrl.handle_key('f', cz::KeyCode::Char, now);
    assert(ctrl.state().filter == ef);
  }

  // R toggles 8MP <-> 2MP and reconfigures backend
  const size_t reconf_before = spy->reconfigure_specs.size();
  ctrl.handle_key('r', cz::KeyCode::Char, now);
  assert(ctrl.state().config.photo_resolution == "2mp");
  assert(spy->reconfigure_specs.size() == reconf_before + 1);
  assert(spy->reconfigure_specs.back().main_width == 1640);
  assert(spy->reconfigure_specs.back().main_height == 1232);
  ctrl.handle_key('R', cz::KeyCode::Char, now);
  assert(ctrl.state().config.photo_resolution == "8mp");
  assert(spy->reconfigure_specs.back().main_width == 3280);
  assert(spy->reconfigure_specs.back().main_height == 2464);

  // C cycles Off -> 3s -> 10s -> Off; Esc cancels an active countdown
  ctrl.handle_key('c', cz::KeyCode::Char, now);
  assert(ctrl.state().config.photo_timer_sec == 3);
  ctrl.handle_key('c', cz::KeyCode::Char, now);
  assert(ctrl.state().config.photo_timer_sec == 10);
  ctrl.handle_key('c', cz::KeyCode::Char, now);
  assert(ctrl.state().config.photo_timer_sec == 0);

  ctrl.handle_key('c', cz::KeyCode::Char, now);  // 3s
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.state().timer_active);
  assert(ctrl.state().timer_remaining_sec == 3);
  now += 1500;
  ctrl.tick(now);
  assert(ctrl.state().timer_active);
  assert(ctrl.state().timer_remaining_sec == 2);
  ctrl.handle_key(0, cz::KeyCode::Esc, now);
  assert(!ctrl.state().timer_active);
  assert(!ctrl.should_quit());
  assert(count_files_with_ext(out_dir, ".jpg") == 0);

  // Set timer back to Off, switch to Invert filter, and shoot via Enter and '6'
  ctrl.handle_key('c', cz::KeyCode::Char, now);  // 10
  ctrl.handle_key('c', cz::KeyCode::Char, now);  // 0
  assert(ctrl.state().config.photo_timer_sec == 0);
  ctrl.handle_key('f', cz::KeyCode::Char, now);  // BW

  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.consume_capture_flash());
  assert(!ctrl.consume_capture_flash());
  assert(count_files_with_ext(out_dir, ".jpg") == 1);
  assert(count_hidden_files(out_dir) == 0);

  now += 1000;
  ctrl.handle_key('6', cz::KeyCode::Char, now);
  assert(ctrl.consume_capture_flash());
  assert(count_files_with_ext(out_dir, ".jpg") == 2);
  assert(count_hidden_files(out_dir) == 0);

  // Photo rail_cursor == 1 adjusts EV across [-2.0, +2.0] in 0.5 steps
  assert(ctrl.state().rail_cursor == 1);
  assert(ctrl.state().config.ev == 0.0);
  for (int i = 1; i <= 5; ++i) {
    ctrl.handle_key(0, cz::KeyCode::Right, now);
  }
  assert(ctrl.state().config.ev == 2.0);
  assert(spy->last_ev == 2.0f);
  for (int i = 1; i <= 9; ++i) {
    ctrl.handle_key(0, cz::KeyCode::Left, now);
  }
  assert(ctrl.state().config.ev == -2.0);
  assert(spy->last_ev == -2.0f);
  for (int i = 1; i <= 4; ++i) {
    ctrl.handle_key(0, cz::KeyCode::Right, now);
  }
  assert(ctrl.state().config.ev == 0.0);
  assert(cz::load_config(cfg_path).ev == 0.0);
}

void test_rail_side_and_theme_toggles_persist() {
  TempDir tmp("rail_theme");
  const std::string cfg_path = tmp.path + "/config.json";
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  cz::AppConfig cfg;
  cfg.output_dir = out_dir;
  assert(cz::save_config(cfg, cfg_path));

  cz::UiControllerOptions opts;
  opts.config_path = cfg_path;
  opts.backlight_sysfs_root = tmp.path + "/no_backlight";

  cz::UiController ctrl(std::make_unique<cz::SimCameraBackend>(), opts);
  assert(ctrl.init(1000));
  assert(ctrl.state().config.rail_side == "right");
  assert(ctrl.state().theme == cz::ThemeMode::Dark);

  ctrl.handle_key('w', cz::KeyCode::Char, 1010);
  assert(ctrl.state().config.rail_side == "left");
  assert(cz::load_config(cfg_path).rail_side == "left");

  ctrl.handle_key('t', cz::KeyCode::Char, 1020);
  assert(ctrl.state().theme == cz::ThemeMode::Light);
  assert(ctrl.state().config.theme == "light");
  assert(cz::load_config(cfg_path).theme == "light");

  // Render check: Dark+Right vs Light+Left must differ
  cz::PreviewFrame live;
  assert(ctrl.backend()->copy_latest_preview(0, live));

  cz::AppView view;
  view.apply_theme(ctrl.state().theme);
  view.render(ctrl.state(), &live, nullptr);
  std::vector<uint32_t> light_left(
      view.pixels(),
      view.pixels() + cz::AppView::kWidth * cz::AppView::kHeight);

  ctrl.handle_key('W', cz::KeyCode::Char, 1030);
  assert(ctrl.state().config.rail_side == "right");
  assert(cz::load_config(cfg_path).rail_side == "right");

  ctrl.handle_key('T', cz::KeyCode::Char, 1040);
  assert(ctrl.state().theme == cz::ThemeMode::Dark);
  assert(cz::load_config(cfg_path).theme == "dark");

  view.apply_theme(ctrl.state().theme);
  view.render(ctrl.state(), &live, nullptr);
  std::vector<uint32_t> dark_right(
      view.pixels(),
      view.pixels() + cz::AppView::kWidth * cz::AppView::kHeight);
  assert(light_left != dark_right);
}

void test_video_mode_locks_transitions_while_recording() {
  TempDir tmp("video");
  const std::string cfg_path = tmp.path + "/config.json";
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  cz::AppConfig cfg;
  cfg.output_dir = out_dir;
  assert(cz::save_config(cfg, cfg_path));

  cz::UiControllerOptions opts;
  opts.config_path = cfg_path;
  opts.backlight_sysfs_root = tmp.path + "/no_backlight";

  cz::UiController ctrl(std::make_unique<cz::SimCameraBackend>(), opts);
  uint64_t now = 1000;
  assert(ctrl.init(now));

  ctrl.handle_key('2', cz::KeyCode::Char, now);
  assert(ctrl.state().mode == cz::AppMode::Video);
  assert(ctrl.state().config.video_audio);

  ctrl.handle_key('m', cz::KeyCode::Char, now);
  assert(!ctrl.state().config.video_audio);
  ctrl.handle_key('M', cz::KeyCode::Char, now);
  assert(ctrl.state().config.video_audio);

  // Video rail Card 3 (FREE) is read-only and must not mutate EV
  const double ev_before = ctrl.state().config.ev;
  while (ctrl.state().rail_cursor < 3) {
    ctrl.handle_key(0, cz::KeyCode::Down, now);
  }
  assert(ctrl.state().rail_cursor == 3);
  ctrl.handle_key(0, cz::KeyCode::Left, now);
  assert(ctrl.state().config.ev == ev_before);
  ctrl.handle_key(0, cz::KeyCode::Right, now);
  assert(ctrl.state().config.ev == ev_before);
  ctrl.handle_key('h', cz::KeyCode::Char, now);
  assert(ctrl.state().config.ev == ev_before);
  ctrl.handle_key('l', cz::KeyCode::Char, now);
  assert(ctrl.state().config.ev == ev_before);
  assert(cz::load_config(cfg_path).ev == ev_before);

  // Start recording with Enter
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.state().video_recording);

  // Locked keys while recording must be ignored cleanly
  const char locked_keys[] = {'1', '3', '8', 's', 'w', 'm', 'r', 't', 'q'};
  for (char ch : locked_keys) {
    ctrl.handle_key(ch, cz::KeyCode::Char, now);
    assert(ctrl.state().mode == cz::AppMode::Video);
    assert(ctrl.state().modal == cz::OverlayModal::None);
    assert(ctrl.state().config.rail_side == "right");
    assert(ctrl.state().config.video_audio);
    assert(ctrl.state().config.video_resolution == "720p");
    assert(ctrl.state().theme == cz::ThemeMode::Dark);
    assert(ctrl.state().video_recording);
    assert(!ctrl.should_quit());
  }

  // Zoom 5/7 still works live during recording
  ctrl.handle_key('7', cz::KeyCode::Char, now);
  assert(ctrl.state().zoom == 1.5f);

  now += 1500;
  ctrl.tick(now);
  assert(ctrl.state().video_elapsed_sec >= 1);

  // Esc stops recording without quitting
  ctrl.handle_key(0, cz::KeyCode::Esc, now);
  assert(!ctrl.state().video_recording);
  assert(!ctrl.should_quit());
  assert(count_files_with_ext(out_dir, ".mp4") == 1);
  assert(count_hidden_files(out_dir) == 0);

  // Enter also starts & stops recording cleanly
  now += 500;
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.state().video_recording);
  now += 500;
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(!ctrl.state().video_recording);
  assert(count_files_with_ext(out_dir, ".mp4") == 2);
}

void test_timelapse_dimmed_safe_wake() {
  auto run_wake_case = [](const std::string& label,
                          char wake_ch,
                          cz::KeyCode wake_code,
                          bool finish_with_stop_confirm) {
    TempDir tmp("tl_wake_" + label);
    const std::string cfg_path = tmp.path + "/config.json";
    const std::string out_dir = tmp.path + "/out";
    const std::string bl_root = tmp.path + "/backlight";
    const std::string lcd0 = bl_root + "/lcd0";
    assert(cz::mkdir_p(out_dir));
    assert(cz::mkdir_p(lcd0));
    write_text(lcd0 + "/brightness", "128\n");
    write_text(lcd0 + "/max_brightness", "255\n");

    cz::AppConfig cfg;
    cfg.output_dir = out_dir;
    cfg.timelapse_dim_sec = 5;
    cfg.timelapse_interval_sec = 4;
    cfg.timelapse_output_mode = "video";
    cfg.timelapse_resolution = "2mp";
    assert(cz::save_config(cfg, cfg_path));

    cz::UiControllerOptions opts;
    opts.config_path = cfg_path;
    opts.backlight_sysfs_root = bl_root;

    cz::UiController ctrl(std::make_unique<cz::SimCameraBackend>(), opts);
    uint64_t now = 1000;
    assert(ctrl.init(now));
    assert(ctrl.backlight().has_hardware_backlight());

    ctrl.handle_key('3', cz::KeyCode::Char, now);
    assert(ctrl.state().mode == cz::AppMode::Timelapse);

    ctrl.handle_key(0, cz::KeyCode::Enter, now);
    ctrl.tick(now);
    assert(ctrl.state().timelapse.running);
    assert(ctrl.state().timelapse.shots_taken >= 1);
    assert(!ctrl.backlight().is_dimmed());

    // Advance 6s (> 5s dim threshold)
    now += 6000;
    ctrl.tick(now);
    assert(ctrl.backlight().is_dimmed());
    assert(ctrl.state().timelapse_dimmed);
    assert(std::stoi(read_text(lcd0 + "/brightness")) == 0);

    // First keypress while dimmed ONLY wakes backlight
    ctrl.handle_key(wake_ch, wake_code, now);
    assert(!ctrl.backlight().is_dimmed());
    assert(!ctrl.state().timelapse_dimmed);
    assert(std::stoi(read_text(lcd0 + "/brightness")) == 128);
    assert(ctrl.state().modal == cz::OverlayModal::None);
    assert(ctrl.state().timelapse.running);
    assert(!ctrl.should_quit());

    if (finish_with_stop_confirm) {
      // Pressing Esc while awake opens TimelapseStopConfirm
      now += 50;
      ctrl.handle_key(0, cz::KeyCode::Esc, now);
      assert(ctrl.state().modal == cz::OverlayModal::TimelapseStopConfirm);
      assert(ctrl.state().timelapse.running);

      // [N] resumes capture
      now += 50;
      ctrl.handle_key('n', cz::KeyCode::Char, now);
      assert(ctrl.state().modal == cz::OverlayModal::None);
      assert(ctrl.state().timelapse.running);

      // Esc again -> [Y] stops & compiles MP4
      now += 50;
      ctrl.handle_key(0, cz::KeyCode::Esc, now);
      assert(ctrl.state().modal == cz::OverlayModal::TimelapseStopConfirm);
      now += 50;
      ctrl.handle_key('y', cz::KeyCode::Char, now);
      assert(!ctrl.state().timelapse.running);

      const auto dl =
          std::chrono::steady_clock::now() + std::chrono::seconds(8);
      while (ctrl.state().modal == cz::OverlayModal::TimelapseCompiling &&
             std::chrono::steady_clock::now() < dl) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        now += 20;
        ctrl.tick(now);
      }
      assert(ctrl.state().modal == cz::OverlayModal::None);
      assert(!ctrl.state().timelapse.compiling);
      assert(count_files_with_ext(out_dir, ".mp4") == 1);
      assert(count_hidden_files(out_dir) == 0);
    } else {
      // Clean stop before teardown
      now += 50;
      ctrl.handle_key(0, cz::KeyCode::Esc, now);
      ctrl.handle_key('y', cz::KeyCode::Char, now);
      const auto dl =
          std::chrono::steady_clock::now() + std::chrono::seconds(8);
      while (ctrl.state().modal == cz::OverlayModal::TimelapseCompiling &&
             std::chrono::steady_clock::now() < dl) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        now += 20;
        ctrl.tick(now);
      }
    }
  };

  run_wake_case("enter", 0, cz::KeyCode::Enter, false);
  run_wake_case("6", '6', cz::KeyCode::Char, false);
  run_wake_case("esc", 0, cz::KeyCode::Esc, false);
  run_wake_case("q", 'q', cz::KeyCode::Char, true);
}

void test_settings_dir_picker_camera_orient_and_gallery_delete() {
  TempDir tmp("settings_gallery");
  const std::string cfg_path = tmp.path + "/config.json";
  const std::string out1 = tmp.path + "/out1";
  const std::string alt_parent = tmp.path + "/alt_dir";
  const std::string picked_sub = alt_parent + "/picked_sub";
  assert(cz::mkdir_p(out1));
  assert(cz::mkdir_p(picked_sub));

  cz::AppConfig cfg;
  cfg.output_dir = out1;
  assert(cz::save_config(cfg, cfg_path));

  auto spy_owned = std::make_unique<SpyCameraBackend>();
  SpyCameraBackend* spy = spy_owned.get();

  cz::UiControllerOptions opts;
  opts.config_path = cfg_path;
  opts.backlight_sysfs_root = tmp.path + "/no_backlight";

  cz::UiController ctrl(std::move(spy_owned), opts);
  uint64_t now = 1000;
  assert(ctrl.init(now));

  // [S] opens Settings
  ctrl.handle_key('s', cz::KeyCode::Char, now);
  assert(ctrl.state().modal == cz::OverlayModal::Settings);
  assert(ctrl.state().settings_cursor == 0);

  // Navigate to Camera Orient (row 5) and cycle auto -> 0 -> 180 -> hflip -> vflip
  for (int i = 0; i < 5; ++i) {
    ctrl.handle_key(0, cz::KeyCode::Down, now);
  }
  assert(ctrl.state().settings_cursor == 5);

  const std::string expected_orients[] = {"0", "180", "hflip", "vflip", "auto"};
  for (const std::string& exp : expected_orients) {
    const size_t n_before = spy->reconfigure_specs.size();
    ctrl.handle_key(0, cz::KeyCode::Enter, now);
    assert(ctrl.state().config.camera_transform == exp);
    assert(spy->reconfigure_specs.size() == n_before + 1);
    assert(spy->reconfigure_specs.back().camera_transform == exp);
  }

  // Navigate back to Output Dir (row 0) and open DirPicker
  for (int i = 0; i < 5; ++i) {
    ctrl.handle_key(0, cz::KeyCode::Up, now);
  }
  assert(ctrl.state().settings_cursor == 0);
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.state().modal == cz::OverlayModal::DirPicker);
  assert(ctrl.state().dir_current_path == fs::weakly_canonical(out1).string());

  // Up to tmp.path, find alt_dir, Enter into alt_dir, select picked_sub with Space
  ctrl.handle_key(0, cz::KeyCode::Backspace, now);
  assert(ctrl.state().dir_current_path == fs::weakly_canonical(tmp.path).string());

  int alt_idx = -1;
  for (size_t i = 0; i < ctrl.state().dir_entries.size(); ++i) {
    if (fs::path(ctrl.state().dir_entries[i]).filename().string() == "alt_dir") {
      alt_idx = static_cast<int>(i);
      break;
    }
  }
  assert(alt_idx >= 0);
  while (ctrl.state().dir_cursor < alt_idx) {
    ctrl.handle_key(0, cz::KeyCode::Down, now);
  }
  while (ctrl.state().dir_cursor > alt_idx) {
    ctrl.handle_key(0, cz::KeyCode::Up, now);
  }
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.state().dir_current_path ==
         fs::weakly_canonical(alt_parent).string());
  assert(ctrl.state().dir_entries.size() == 1);
  assert(fs::path(ctrl.state().dir_entries[0]).filename().string() ==
         "picked_sub");

  ctrl.handle_key(' ', cz::KeyCode::Space, now);
  assert(ctrl.state().modal == cz::OverlayModal::Settings);
  assert(ctrl.state().effective_output_dir ==
         fs::weakly_canonical(picked_sub).string());
  assert(cz::load_config(cfg_path).output_dir ==
         fs::weakly_canonical(picked_sub).string());

  // Close Settings
  ctrl.handle_key(0, cz::KeyCode::Esc, now);
  assert(ctrl.state().modal == cz::OverlayModal::None);

  // Capture a photo into picked_sub
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(count_files_with_ext(picked_sub, ".jpg") == 1);

  // [8] pauses backend and opens Gallery
  assert(spy->pause_calls == 0);
  ctrl.handle_key('8', cz::KeyCode::Char, now);
  assert(ctrl.state().modal == cz::OverlayModal::Gallery);
  assert(spy->pause_calls == 1);
  assert(ctrl.state().gallery_items.size() == 1);
  assert(ctrl.gallery_preview() != nullptr);
  assert(!ctrl.gallery_preview()->rgb24.empty());

  // [D] requires [Y] before deleting
  ctrl.handle_key('d', cz::KeyCode::Char, now);
  assert(ctrl.state().gallery_delete_confirm);
  assert(count_files_with_ext(picked_sub, ".jpg") == 1);

  ctrl.handle_key('n', cz::KeyCode::Char, now);
  assert(!ctrl.state().gallery_delete_confirm);
  assert(count_files_with_ext(picked_sub, ".jpg") == 1);

  ctrl.handle_key('d', cz::KeyCode::Char, now);
  assert(ctrl.state().gallery_delete_confirm);
  ctrl.handle_key('y', cz::KeyCode::Char, now);
  assert(!ctrl.state().gallery_delete_confirm);
  assert(ctrl.state().gallery_items.empty());
  assert(count_files_with_ext(picked_sub, ".jpg") == 0);

  // Esc exits Gallery and resumes backend
  assert(spy->resume_calls == 0);
  ctrl.handle_key(0, cz::KeyCode::Esc, now);
  assert(ctrl.state().modal == cz::OverlayModal::None);
  assert(spy->resume_calls == 1);
}

void test_text_input_timelapse_custom_recovery_and_modal_renders() {
  TempDir tmp("modals");
  const std::string cfg_path = tmp.path + "/config.json";
  const std::string out_dir = tmp.path + "/out";
  const std::string typed_dir = tmp.path + "/typed_out";
  assert(cz::mkdir_p(out_dir));

  cz::AppConfig cfg;
  cfg.output_dir = out_dir;
  assert(cz::save_config(cfg, cfg_path));

  // Create an orphaned timelapse work directory to exercise TimelapseRecovery
  const cz::TimelapseSessionPaths orphan_paths =
      cz::create_timelapse_session(out_dir, true);
  {
    cz::SimCameraBackend sim;
    cz::CameraStreamSpec sp;
    std::string err;
    assert(sim.start(sp, err));
    const cz::StagedPaths f1 =
        cz::next_timelapse_frame_paths(orphan_paths.work_dir, 1);
    assert(sim.capture_still(f1.tmp_path, 320, 240).ok);
    assert(cz::commit_staged_file(f1.tmp_path, f1.final_path, err));
    sim.stop();
  }
  cz::TimelapseManifest m;
  m.session_id = orphan_paths.session_id;
  m.work_dir = orphan_paths.work_dir;
  m.resolution = "2mp";
  m.interval_sec = 5;
  m.fps = 24;
  m.frame_count = 1;
  m.status = "recording";
  assert(cz::write_timelapse_manifest(orphan_paths.manifest_path, m));

  cz::UiControllerOptions opts;
  opts.config_path = cfg_path;
  opts.backlight_sysfs_root = tmp.path + "/no_backlight";

  cz::UiController ctrl(std::make_unique<cz::SimCameraBackend>(), opts);
  uint64_t now = 1000;
  assert(ctrl.init(now));

  // Startup detects orphan and opens TimelapseRecovery modal
  assert(ctrl.state().modal == cz::OverlayModal::TimelapseRecovery);
  cz::AppView view;
  cz::PreviewFrame live;
  assert(ctrl.backend()->copy_latest_preview(0, live));
  view.render(ctrl.state(), &live, nullptr);

  // [K] Keep as Photos Folder -> moves to TL_<id>/ and closes recovery modal
  ctrl.handle_key('k', cz::KeyCode::Char, now);
  assert(ctrl.state().modal == cz::OverlayModal::None);
  assert(fs::is_directory(orphan_paths.photos_dir));
  assert(!fs::exists(orphan_paths.work_dir));

  // Gallery drill-down into TL_<id>/ and Backspace back to root
  ctrl.handle_key('8', cz::KeyCode::Char, now);
  assert(ctrl.state().modal == cz::OverlayModal::Gallery);
  assert(ctrl.state().gallery_items.size() == 1);
  assert(ctrl.state().gallery_items[0].type == cz::GalleryItemType::TimelapseFolder);
  view.render(ctrl.state(), &live, ctrl.gallery_preview());
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.state().gallery_in_session);
  assert(ctrl.state().gallery_items.size() == 1);
  view.render(ctrl.state(), &live, ctrl.gallery_preview());
  ctrl.handle_key(0, cz::KeyCode::Backspace, now);
  assert(!ctrl.state().gallery_in_session);
  ctrl.handle_key(0, cz::KeyCode::Esc, now);
  assert(ctrl.state().modal == cz::OverlayModal::None);

  // TimelapseCustomInput [C] editing interval/fps/stop mode
  ctrl.handle_key('3', cz::KeyCode::Char, now);
  ctrl.handle_key('c', cz::KeyCode::Char, now);
  assert(ctrl.state().modal == cz::OverlayModal::TimelapseCustomInput);
  ctrl.handle_key('1', cz::KeyCode::Char, now);
  ctrl.handle_key('2', cz::KeyCode::Char, now);
  assert(ctrl.state().tl_custom_interval_sec == 12);
  ctrl.handle_key(0, cz::KeyCode::Down, now);
  ctrl.handle_key(0, cz::KeyCode::Right, now);  // 24 -> 30 fps
  assert(ctrl.state().tl_custom_fps == 30);
  ctrl.handle_key(0, cz::KeyCode::Down, now);
  ctrl.handle_key(0, cz::KeyCode::Right, now);  // manual -> shots
  assert(ctrl.state().tl_custom_stop_mode == "shots");
  view.render(ctrl.state(), &live, nullptr);
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.state().modal == cz::OverlayModal::None);
  assert(ctrl.state().config.timelapse_interval_sec == 12);
  assert(ctrl.state().config.timelapse_fps == 30);
  assert(ctrl.state().config.timelapse_stop_mode == "shots");

  // Settings -> DirPicker -> [T] TextInput manual directory entry
  ctrl.handle_key('s', cz::KeyCode::Char, now);
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.state().modal == cz::OverlayModal::DirPicker);
  view.render(ctrl.state(), &live, nullptr);
  ctrl.handle_key('t', cz::KeyCode::Char, now);
  assert(ctrl.state().modal == cz::OverlayModal::TextInput);
  for (char c : typed_dir) {
    ctrl.handle_key(c, cz::KeyCode::Char, now);
  }
  view.render(ctrl.state(), &live, nullptr);
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  assert(ctrl.state().modal == cz::OverlayModal::Settings);
  assert(ctrl.state().effective_output_dir == typed_dir);
  assert(fs::is_directory(typed_dir));
  ctrl.handle_key(0, cz::KeyCode::Esc, now);

  // Software black screen when dimmed without hardware backlight
  cz::UiState dimmed_sw = ctrl.state();
  dimmed_sw.timelapse_dimmed = true;
  dimmed_sw.has_hardware_backlight = false;
  view.render(dimmed_sw, &live, nullptr);
  const uint32_t* px = view.pixels();
  for (int i = 0; i < cz::AppView::kWidth * cz::AppView::kHeight; ++i) {
    assert(px[i] == 0xff000000U);
  }
}

void test_backend_drop_toast_and_stale_live_frame_rendering() {
  TempDir tmp("backend_drop");
  const std::string cfg_path = tmp.path + "/config.json";
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  cz::AppConfig cfg;
  cfg.output_dir = out_dir;
  assert(cz::save_config(cfg, cfg_path));

  cz::UiControllerOptions opts;
  opts.config_path = cfg_path;
  opts.backlight_sysfs_root = tmp.path + "/no_backlight";
  opts.skip_startup_recovery_scan = true;

  // 1. Initial startup failure surfaces startup toast once without spurious repeats
  {
    auto fail_owned = std::make_unique<DropSimBackend>();
    fail_owned->fail_start = true;
    cz::UiController ctrl(std::move(fail_owned), opts);
    uint64_t now = 1000;
    assert(ctrl.init(now));
    assert(!ctrl.state().backend_ready);
    assert(ctrl.state().toast_is_error);
    assert(ctrl.state().toast_message == "Simulated camera start failure");
    now += 4000;
    ctrl.tick(now);
    assert(ctrl.state().toast_message.empty());
  }

  // 2. Unexpected true -> false drop emits error toast, and offline/paused
  //    render cleanly even when a non-null stale live_frame is passed.
  auto drop_owned = std::make_unique<DropSimBackend>();
  DropSimBackend* drop = drop_owned.get();
  cz::UiController ctrl(std::move(drop_owned), opts);
  uint64_t now = 1000;
  assert(ctrl.init(now));
  assert(ctrl.state().backend_ready);
  assert(!ctrl.state().backend_paused);
  assert(ctrl.state().toast_message.empty());

  cz::PreviewFrame live;
  assert(ctrl.backend()->copy_latest_preview(0, live));
  assert(!live.rgb24.empty());

  cz::AppView view;
  view.apply_theme(ctrl.state().theme);
  view.render(ctrl.state(), &live, nullptr);
  const std::vector<uint32_t> live_pixels(
      view.pixels(),
      view.pixels() + cz::AppView::kWidth * cz::AppView::kHeight);

  // Stale 16:9 flag in stale live_frame must not affect offline/paused Photo layout
  cz::PreviewFrame stale_16_9 = live;
  stale_16_9.is_16_9 = true;

  cz::UiState offline_state = ctrl.state();
  offline_state.backend_ready = false;
  offline_state.backend_paused = false;
  view.render(offline_state, &live, nullptr);
  const std::vector<uint32_t> offline_with_4_3(
      view.pixels(),
      view.pixels() + cz::AppView::kWidth * cz::AppView::kHeight);
  view.render(offline_state, &stale_16_9, nullptr);
  const std::vector<uint32_t> offline_with_stale(
      view.pixels(),
      view.pixels() + cz::AppView::kWidth * cz::AppView::kHeight);
  view.render(offline_state, nullptr, nullptr);
  const std::vector<uint32_t> offline_with_null(
      view.pixels(),
      view.pixels() + cz::AppView::kWidth * cz::AppView::kHeight);
  assert(offline_with_4_3 != live_pixels);
  assert(offline_with_4_3 == offline_with_null);
  assert(offline_with_stale != live_pixels);
  assert(offline_with_stale == offline_with_null);

  cz::UiState paused_state = ctrl.state();
  paused_state.backend_ready = true;
  paused_state.backend_paused = true;
  view.render(paused_state, &live, nullptr);
  const std::vector<uint32_t> paused_with_4_3(
      view.pixels(),
      view.pixels() + cz::AppView::kWidth * cz::AppView::kHeight);
  view.render(paused_state, &stale_16_9, nullptr);
  const std::vector<uint32_t> paused_with_stale(
      view.pixels(),
      view.pixels() + cz::AppView::kWidth * cz::AppView::kHeight);
  view.render(paused_state, nullptr, nullptr);
  const std::vector<uint32_t> paused_with_null(
      view.pixels(),
      view.pixels() + cz::AppView::kWidth * cz::AppView::kHeight);
  assert(paused_with_4_3 != live_pixels);
  assert(paused_with_4_3 == paused_with_null);
  assert(paused_with_stale != live_pixels);
  assert(paused_with_stale != offline_with_stale);
  assert(paused_with_stale == paused_with_null);

  // Trigger unexpected backend drop with descriptive backend error
  drop->simulate_drop("czcamera-cam exited unexpectedly (code 1)");
  now += 100;
  ctrl.tick(now);
  assert(!ctrl.state().backend_ready);
  assert(ctrl.state().toast_is_error);
  assert(ctrl.state().toast_message ==
         "czcamera-cam exited unexpectedly (code 1)");

  // Recover via [R], verify fallback "Camera offline" when last_error is empty
  now += 100;
  ctrl.handle_key('r', cz::KeyCode::Char, now);
  assert(ctrl.state().backend_ready);
  assert(ctrl.state().toast_message == "Camera recovered");
  now += 4000;
  ctrl.tick(now);
  assert(ctrl.state().toast_message.empty());

  drop->simulate_drop("");
  now += 100;
  ctrl.tick(now);
  assert(!ctrl.state().backend_ready);
  assert(ctrl.state().toast_is_error);
  assert(ctrl.state().toast_message == "Camera offline");

  // Recover and confirm shutdown() leaves no spurious error toast
  now += 100;
  ctrl.handle_key('r', cz::KeyCode::Char, now);
  assert(ctrl.state().backend_ready);
  now += 4000;
  ctrl.tick(now);
  assert(ctrl.state().toast_message.empty());
  ctrl.shutdown();
  assert(!ctrl.state().backend_ready);
  assert(ctrl.state().toast_message.empty());
}

void test_timelapse_photos_auto_finish_clears_stop_confirm_modal() {
  TempDir tmp("tl_stop_confirm_autofinish");
  const std::string cfg_path = tmp.path + "/config.json";
  const std::string out_dir = tmp.path + "/out";
  assert(cz::mkdir_p(out_dir));

  cz::AppConfig cfg;
  cfg.output_dir = out_dir;
  cfg.timelapse_output_mode = "photos";
  cfg.timelapse_interval_sec = 2;
  cfg.timelapse_stop_mode = "shots";
  cfg.timelapse_stop_value = 2;
  assert(cz::save_config(cfg, cfg_path));

  cz::UiControllerOptions opts;
  opts.config_path = cfg_path;
  opts.backlight_sysfs_root = tmp.path + "/no_backlight";
  opts.skip_startup_recovery_scan = true;

  cz::UiController ctrl(std::make_unique<cz::SimCameraBackend>(), opts);
  uint64_t now = 1000;
  assert(ctrl.init(now));

  ctrl.handle_key('3', cz::KeyCode::Char, now);
  assert(ctrl.state().mode == cz::AppMode::Timelapse);
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  ctrl.tick(now);
  assert(ctrl.state().timelapse.running);
  assert(ctrl.state().timelapse.shots_taken == 1);

  // Open Stop Confirm modal while Photos-mode timelapse is active
  now += 100;
  ctrl.handle_key(0, cz::KeyCode::Esc, now);
  assert(ctrl.state().modal == cz::OverlayModal::TimelapseStopConfirm);

  // Tick past the 2-shot stop limit; modal must clear automatically
  now += 2500;
  ctrl.tick(now);
  assert(!ctrl.state().timelapse.running);
  assert(!ctrl.state().timelapse.needs_compile);
  assert(ctrl.state().timelapse.shots_taken == 2);
  assert(ctrl.state().modal == cz::OverlayModal::None);
}

void test_evdev_repeat_guard_suppresses_wake_key_repeats() {
  TempDir tmp("evdev_repeat_guard");
  const std::string cfg_path = tmp.path + "/config.json";
  const std::string out_dir = tmp.path + "/out";
  const std::string bl_root = tmp.path + "/backlight";
  const std::string lcd0 = bl_root + "/lcd0";
  assert(cz::mkdir_p(out_dir));
  assert(cz::mkdir_p(lcd0));
  write_text(lcd0 + "/brightness", "128\n");
  write_text(lcd0 + "/max_brightness", "255\n");

  cz::AppConfig cfg;
  cfg.output_dir = out_dir;
  cfg.timelapse_dim_sec = 5;
  cfg.timelapse_interval_sec = 4;
  cfg.timelapse_output_mode = "photos";
  assert(cz::save_config(cfg, cfg_path));

  cz::UiControllerOptions opts;
  opts.config_path = cfg_path;
  opts.backlight_sysfs_root = bl_root;
  opts.skip_startup_recovery_scan = true;

  cz::UiController ctrl(std::make_unique<cz::SimCameraBackend>(), opts);
  uint64_t now = 1000;
  assert(ctrl.init(now));

  ctrl.handle_key('3', cz::KeyCode::Char, now);
  ctrl.handle_key(0, cz::KeyCode::Enter, now);
  ctrl.tick(now);
  assert(ctrl.state().timelapse.running);

  now += 6000;
  ctrl.tick(now);
  assert(ctrl.state().timelapse_dimmed);

  cz::EvdevRepeatGuard guard;
  constexpr uint16_t kKeyEsc = 1;
  constexpr uint16_t kKeyDown = 108;

  // Initial press (value=1) while dimmed wakes LCD and arms repeat suppression
  assert(guard.should_dispatch(kKeyEsc, 1, ctrl.state().timelapse_dimmed));
  ctrl.handle_key(0, cz::KeyCode::Esc, now);
  assert(!ctrl.state().timelapse_dimmed);
  assert(ctrl.state().modal == cz::OverlayModal::None);

  // Kernel auto-repeats (value=2) while key stays held must be suppressed
  now += 250;
  assert(!guard.should_dispatch(kKeyEsc, 2, ctrl.state().timelapse_dimmed));
  assert(!guard.should_dispatch(kKeyEsc, 2, ctrl.state().timelapse_dimmed));
  assert(ctrl.state().modal == cz::OverlayModal::None);

  // Normal repeats for a key pressed while awake still dispatch
  assert(guard.should_dispatch(kKeyDown, 1, ctrl.state().timelapse_dimmed));
  assert(guard.should_dispatch(kKeyDown, 2, ctrl.state().timelapse_dimmed));
  assert(!guard.should_dispatch(kKeyDown, 0, ctrl.state().timelapse_dimmed));

  // Key release (value=0) clears suppression; next fresh press opens modal
  assert(!guard.should_dispatch(kKeyEsc, 0, ctrl.state().timelapse_dimmed));
  now += 50;
  assert(guard.should_dispatch(kKeyEsc, 1, ctrl.state().timelapse_dimmed));
  ctrl.handle_key(0, cz::KeyCode::Esc, now);
  assert(ctrl.state().modal == cz::OverlayModal::TimelapseStopConfirm);
}

}  // namespace

int main() {
  test_photo_controls_zoom_loupe_filters_and_timer();
  test_rail_side_and_theme_toggles_persist();
  test_video_mode_locks_transitions_while_recording();
  test_timelapse_dimmed_safe_wake();
  test_settings_dir_picker_camera_orient_and_gallery_delete();
  test_text_input_timelapse_custom_recovery_and_modal_renders();
  test_backend_drop_toast_and_stale_live_frame_rendering();
  test_timelapse_photos_auto_finish_clears_stop_confirm_modal();
  test_evdev_repeat_guard_suppresses_wake_key_repeats();
  std::cout << "test_ui_controller: ALL PASSED\n";
  return 0;
}
