#include "ui_controller.hpp"

#include "jpeg_pipeline.hpp"
#include "storage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace cz {

namespace {

constexpr uint64_t kMinStorageBytes = 104857600ULL;  // 100 MB
constexpr uint64_t kCaptureFlashMs = 150ULL;
constexpr uint64_t kToastDurationMs = 3000ULL;
constexpr std::array<float, 5> kZoomSteps = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
constexpr std::array<int, 6> kIntervalPresets = {2, 5, 10, 30, 60, 300};
constexpr std::array<int, 4> kDimPresets = {15, 30, 60, 0};
constexpr std::array<const char*, 5> kOrientPresets = {"auto", "0", "180",
                                                       "hflip", "vflip"};

struct StopPreset {
  const char* mode;
  int value;
};
constexpr std::array<StopPreset, 7> kStopPresets = {{
    {"manual", 0},
    {"shots", 60},
    {"shots", 120},
    {"shots", 300},
    {"duration", 900},
    {"duration", 1800},
    {"duration", 3600},
}};

std::string to_lower_ascii(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

std::string trim_ascii(const std::string& s) {
  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }
  size_t end = s.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(start, end - start);
}

std::string base_name(const std::string& path) {
  if (path.empty()) return "";
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') {
    p.pop_back();
  }
  const size_t slash = p.find_last_of('/');
  return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

std::string parent_dir_of(const std::string& path) {
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') {
    p.pop_back();
  }
  const size_t slash = p.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return p.substr(0, slash);
}

bool is_valid_dir(const std::string& path) {
  if (path.empty()) return false;
  std::error_code ec;
  return fs::is_directory(fs::path(path), ec);
}

void remove_silent(const std::string& path) {
  if (path.empty()) return;
  std::error_code ec;
  fs::remove(path, ec);
}

KeyCode normalize_key(char ch, KeyCode code) {
  if (code != KeyCode::Char) {
    return code;
  }
  if (ch == ' ') return KeyCode::Space;
  if (ch == '\n' || ch == '\r') return KeyCode::Enter;
  if (ch == '\b' || static_cast<unsigned char>(ch) == 0x7f) {
    return KeyCode::Backspace;
  }
  if (static_cast<unsigned char>(ch) == 0x1b) return KeyCode::Esc;
  return KeyCode::Char;
}

void photo_dims(const std::string& res, int& w, int& h) {
  if (to_lower_ascii(res) == "2mp") {
    w = 1640;
    h = 1232;
  } else {
    w = 3280;
    h = 2464;
  }
}

void video_params(const std::string& res, int& w, int& h, int& bitrate) {
  const std::string r = to_lower_ascii(res);
  if (r == "1080p") {
    w = 1920;
    h = 1080;
    bitrate = 8000000;
  } else if (r == "480p") {
    w = 854;
    h = 480;
    bitrate = 2000000;
  } else {
    w = 1280;
    h = 720;
    bitrate = 4000000;
  }
}

void timelapse_dims(const std::string& res, int& w, int& h, bool& is_16_9) {
  const std::string r = to_lower_ascii(res);
  if (r == "8mp") {
    w = 3280;
    h = 2464;
    is_16_9 = false;
  } else if (r == "1080p") {
    w = 1920;
    h = 1080;
    is_16_9 = true;
  } else {
    w = 1640;
    h = 1232;
    is_16_9 = false;
  }
}

template <size_t N>
int find_index_str(const std::array<const char*, N>& arr,
                   const std::string& val) {
  const std::string low = to_lower_ascii(val);
  for (size_t i = 0; i < N; ++i) {
    if (low == arr[i]) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

template <size_t N>
int find_index_int(const std::array<int, N>& arr, int val) {
  for (size_t i = 0; i < N; ++i) {
    if (arr[i] == val) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

TimelapseSessionPaths session_paths_from_manifest(
    const TimelapseManifest& manifest) {
  std::string work_dir = manifest.work_dir;
  while (work_dir.size() > 1 && work_dir.back() == '/') {
    work_dir.pop_back();
  }
  const std::string out_dir = parent_dir_of(work_dir);
  std::string session_id = manifest.session_id;
  if (session_id.empty()) {
    const std::string base = base_name(work_dir);
    if (base.rfind(".tl_work_", 0) == 0) {
      session_id = base.substr(9);
    } else {
      session_id = "recovered";
    }
  }
  TimelapseSessionPaths paths;
  paths.session_id = session_id;
  paths.work_dir = work_dir;
  paths.photos_dir = out_dir + "/TL_" + session_id;
  paths.manifest_path = work_dir + "/session.json";
  paths.mp4_tmp_path = out_dir + "/.TL_" + session_id + ".mp4.part";
  paths.mp4_final_path = out_dir + "/TL_" + session_id + ".mp4";
  return paths;
}

}  // namespace

UiController::UiController(std::unique_ptr<ICameraBackend> backend,
                           const UiControllerOptions& opts)
    : opts_(opts),
      backend_(std::move(backend)),
      backlight_(opts.backlight_sysfs_root) {
  if (!backend_) {
    backend_ = create_camera_backend(opts_.mock_backend);
  }
  timelapse_engine_.set_backend(backend_.get());
}

UiController::~UiController() { shutdown(); }

bool UiController::init(uint64_t now_ms) {
  if (initialized_) {
    return true;
  }

  last_now_ms_ = now_ms;
  state_.config = load_config(opts_.config_path);
  persisted_output_dir_ = state_.config.output_dir;

  if (!opts_.output_dir_override.empty()) {
    state_.config.output_dir = opts_.output_dir_override;
    output_dir_override_active_ = true;
  } else {
    output_dir_override_active_ = false;
  }

  state_.effective_output_dir = expand_user_path(state_.config.output_dir);
  state_.theme = theme_from_string(state_.config.theme);
  state_.filter = filter_from_string(state_.config.photo_filter);
  state_.has_hardware_backlight = backlight_.has_hardware_backlight();
  state_.timelapse_dimmed = backlight_.is_dimmed();
  state_.zoom = 1.0f;
  state_.loupe_enabled = false;
  state_.rail_cursor = 1;

  if (!mkdir_p(state_.effective_output_dir)) {
    set_toast("Output dir unavailable", true, now_ms);
  }
  refresh_storage_stats();

  std::string err;
  if (backend_ && !backend_->start(build_stream_spec(), err)) {
    state_.backend_error = err;
    set_toast(err.empty() ? "Camera start failed" : err, true, now_ms);
  }
  sync_backend_status();

  last_activity_ms_ = now_ms;
  initialized_ = true;

  if (!opts_.skip_startup_recovery_scan) {
    orphaned_timelapses_ = scan_orphaned_timelapses(state_.effective_output_dir);
    if (!orphaned_timelapses_.empty()) {
      state_.recovery_manifest = orphaned_timelapses_.front();
      state_.recovery_remaining = static_cast<int>(orphaned_timelapses_.size());
      state_.modal = OverlayModal::TimelapseRecovery;
    }
  }

  return true;
}

void UiController::shutdown() {
  video_player_.stop();
  if (timelapse_engine_.state().running) {
    timelapse_engine_.request_stop();
  }
  timelapse_engine_.cancel_compile();
  join_compile_thread();
  backlight_.restore();
  state_.timelapse_dimmed = false;
  if (backend_) {
    backend_->stop();
  }
  state_.backend_ready = false;
  state_.backend_paused = false;
  initialized_ = false;
}

CameraStreamSpec UiController::build_stream_spec() const {
  CameraStreamSpec spec;
  spec.fps = 30;
  spec.zoom = state_.zoom;
  spec.ev = static_cast<float>(state_.config.ev);
  spec.camera_transform = state_.config.camera_transform;

  switch (state_.mode) {
    case AppMode::Photo: {
      spec.mode = "photo";
      photo_dims(state_.config.photo_resolution, spec.main_width,
                 spec.main_height);
      spec.preview_width = 226;
      spec.preview_height = 170;
      break;
    }
    case AppMode::Video: {
      spec.mode = "video";
      int unused_br = 0;
      video_params(state_.config.video_resolution, spec.main_width,
                   spec.main_height, unused_br);
      spec.preview_width = 226;
      spec.preview_height = 128;
      break;
    }
    case AppMode::Timelapse: {
      spec.mode = "timelapse";
      bool is_16_9 = false;
      timelapse_dims(state_.config.timelapse_resolution, spec.main_width,
                     spec.main_height, is_16_9);
      spec.preview_width = 226;
      spec.preview_height = is_16_9 ? 128 : 170;
      break;
    }
  }
  return spec;
}

AppConfig UiController::effective_engine_config() const {
  AppConfig cfg = state_.config;
  cfg.output_dir = state_.effective_output_dir;
  return cfg;
}

void UiController::persist_config() {
  if (!opts_.persist_config) {
    return;
  }
  AppConfig to_save = state_.config;
  if (output_dir_override_active_) {
    to_save.output_dir = persisted_output_dir_;
  }
  save_config(to_save, opts_.config_path);
}

bool UiController::reconfigure_backend(std::string& out_err) {
  out_err.clear();
  if (!backend_) {
    out_err = "No camera backend";
    return false;
  }
  const bool ok = backend_->reconfigure(build_stream_spec(), out_err);
  if (!ok) {
    state_.backend_error = out_err;
  }
  sync_backend_status();
  return ok;
}

void UiController::sync_backend_status() {
  const bool was_ready = state_.backend_ready;
  if (!backend_) {
    state_.backend_ready = false;
    state_.backend_paused = false;
    if (initialized_ && was_ready) {
      set_toast("Camera offline", true, last_now_ms_);
    }
    return;
  }
  const BackendHealth h = backend_->status();
  state_.backend_ready = h.ready;
  state_.backend_paused = h.paused;
  if (!h.last_error.empty()) {
    state_.backend_error = h.last_error;
  } else if (h.ready) {
    state_.backend_error.clear();
  }

  if (initialized_ && was_ready && !h.ready && !state_.backend_paused) {
    const std::string msg =
        !h.last_error.empty()
            ? h.last_error
            : (!state_.backend_error.empty() ? state_.backend_error
                                             : "Camera offline");
    set_toast(msg, true, last_now_ms_);
  }
}

void UiController::set_toast(const std::string& message,
                             bool is_error,
                             uint64_t now_ms) {
  state_.toast_message = message;
  state_.toast_is_error = is_error && !message.empty();
  toast_until_ms_ = now_ms + kToastDurationMs;
}

void UiController::refresh_storage_stats() {
  uint64_t free_b = 0;
  has_sufficient_storage(state_.effective_output_dir, kMinStorageBytes, &free_b);
  state_.free_bytes = free_b;
}

bool UiController::consume_capture_flash() {
  const bool was = capture_flash_pending_;
  capture_flash_pending_ = false;
  return was;
}

void UiController::join_compile_thread() {
  if (compile_thread_.joinable()) {
    compile_thread_.join();
  }
}

void UiController::start_compile_async(
    const TimelapseSessionPaths& paths,
    int fps,
    const std::string& resolution,
    const TimelapseManifest& fallback_manifest,
    bool from_recovery) {
  join_compile_thread();

  if (backlight_.is_dimmed()) {
    backlight_.restore();
    state_.timelapse_dimmed = false;
  }

  std::string err;
  if (backend_ && !state_.backend_paused) {
    backend_->pause(err);
    sync_backend_status();
  }

  compile_fallback_manifest_ = fallback_manifest;
  compile_from_recovery_ = from_recovery;
  compile_cancelled_by_user_ = false;
  compile_ok_ = false;
  compile_result_msg_.clear();
  compile_finished_.store(false, std::memory_order_release);
  compile_in_flight_.store(true, std::memory_order_release);

  state_.modal = OverlayModal::TimelapseCompiling;
  state_.timelapse.compiling = true;
  state_.timelapse.compile_current = 0;
  state_.timelapse.compile_total = std::max(1, fallback_manifest.frame_count);
  state_.timelapse.compile_progress = 0.0f;

  compile_thread_ = std::thread([this, paths, fps, resolution]() {
    const bool ok = timelapse_engine_.compile_session(paths, fps, resolution);
    {
      std::lock_guard<std::mutex> lk(compile_result_mutex_);
      compile_ok_ = ok;
      if (ok) {
        compile_result_msg_ = "Compiled " + base_name(paths.mp4_final_path);
      } else {
        compile_result_msg_ = timelapse_engine_.state().last_error;
      }
    }
    compile_finished_.store(true, std::memory_order_release);
    compile_in_flight_.store(false, std::memory_order_release);
  });
}

void UiController::finish_compile_if_ready(uint64_t now_ms) {
  if (!compile_finished_.load(std::memory_order_acquire)) {
    return;
  }

  join_compile_thread();
  compile_finished_.store(false, std::memory_order_release);

  bool ok = false;
  std::string msg;
  {
    std::lock_guard<std::mutex> lk(compile_result_mutex_);
    ok = compile_ok_;
    msg = compile_result_msg_;
  }

  if (ok) {
    set_toast(msg, false, now_ms);
  } else if (compile_cancelled_by_user_) {
    std::string rec_msg;
    if (timelapse_engine_.recover_session(compile_fallback_manifest_,
                                          RecoveryAction::KeepPhotos, rec_msg)) {
      set_toast(rec_msg, false, now_ms);
    } else {
      set_toast(rec_msg.empty() ? "Compile cancelled" : rec_msg, true, now_ms);
    }
  } else {
    set_toast(msg.empty() ? "Compile failed" : msg, true, now_ms);
  }

  std::string err;
  if (backend_) {
    backend_->resume(err);
    sync_backend_status();
  }
  refresh_storage_stats();
  state_.timelapse = timelapse_engine_.state();

  if (compile_from_recovery_) {
    advance_recovery_queue();
  } else {
    state_.modal = OverlayModal::None;
  }
}

void UiController::advance_recovery_queue() {
  if (!orphaned_timelapses_.empty()) {
    orphaned_timelapses_.erase(orphaned_timelapses_.begin());
  }
  if (!orphaned_timelapses_.empty()) {
    state_.recovery_manifest = orphaned_timelapses_.front();
    state_.recovery_remaining = static_cast<int>(orphaned_timelapses_.size());
    state_.modal = OverlayModal::TimelapseRecovery;
  } else {
    state_.recovery_manifest = TimelapseManifest{};
    state_.recovery_remaining = 0;
    state_.modal = OverlayModal::None;
  }
}

void UiController::tick(uint64_t now_ms) {
  if (!initialized_) {
    init(now_ms);
  }
  last_now_ms_ = now_ms;

  if (state_.capture_flash_active && now_ms >= flash_until_ms_) {
    state_.capture_flash_active = false;
  }

  if (!state_.toast_message.empty() && now_ms >= toast_until_ms_) {
    state_.toast_message.clear();
    state_.toast_is_error = false;
  }

  if (state_.timer_active) {
    if (now_ms >= timer_target_ms_) {
      state_.timer_active = false;
      state_.timer_remaining_sec = 0;
      capture_photo_now(now_ms);
    } else {
      const uint64_t rem = timer_target_ms_ - now_ms;
      state_.timer_remaining_sec = static_cast<int>((rem + 999ULL) / 1000ULL);
    }
  }

  if (state_.video_recording) {
    state_.video_elapsed_sec =
        (now_ms >= video_start_ms_)
            ? static_cast<int>((now_ms - video_start_ms_) / 1000ULL)
            : 0;
    const BackendHealth h = backend_ ? backend_->status() : BackendHealth{};
    if (h.audio_warning && !warned_audio_) {
      warned_audio_ = true;
      set_toast("Mic unavailable - muted", true, now_ms);
    }
    if (!h.recording) {
      state_.video_recording = false;
      remove_silent(video_tmp_path_);
      set_toast(h.last_error.empty() ? "Video stopped unexpectedly" : h.last_error,
                true, now_ms);
    } else if (now_ms - last_storage_check_ms_ >= 2000ULL) {
      last_storage_check_ms_ = now_ms;
      if (!has_sufficient_storage(state_.effective_output_dir, kMinStorageBytes,
                                  &state_.free_bytes)) {
        stop_video_recording(true, now_ms);
      }
    }
  }

  if (timelapse_engine_.state().running) {
    timelapse_engine_.tick(now_ms, effective_engine_config());
    state_.timelapse = timelapse_engine_.state();

    if (!state_.timelapse.running) {
      backlight_.restore();
      state_.timelapse_dimmed = false;
      if (state_.modal == OverlayModal::TimelapseStopConfirm) {
        state_.modal = OverlayModal::None;
      }
      refresh_storage_stats();

      if (state_.timelapse.needs_compile && !compile_in_flight_.load()) {
        if (state_.timelapse.stopped_by_low_storage) {
          set_toast("Low Storage - Compiling", true, now_ms);
        }
        const TimelapseSessionPaths sp = timelapse_engine_.session_paths();
        TimelapseManifest fb;
        fb.session_id = sp.session_id;
        fb.work_dir = sp.work_dir;
        fb.resolution = state_.config.timelapse_resolution;
        fb.fps = state_.config.timelapse_fps;
        fb.frame_count = state_.timelapse.shots_taken;
        start_compile_async(sp, state_.config.timelapse_fps,
                            state_.config.timelapse_resolution, fb, false);
      } else if (state_.timelapse.stopped_by_low_storage) {
        set_toast("Low Storage - Saved", true, now_ms);
      } else if (!state_.timelapse.last_error.empty()) {
        set_toast(state_.timelapse.last_error, true, now_ms);
      } else if (!state_.timelapse.result_path.empty()) {
        set_toast("Saved " + base_name(state_.timelapse.result_path), false,
                  now_ms);
      }
    } else if (state_.modal == OverlayModal::None &&
               state_.config.timelapse_dim_sec > 0 &&
               !backlight_.is_dimmed()) {
      const uint64_t dim_ms =
          static_cast<uint64_t>(state_.config.timelapse_dim_sec) * 1000ULL;
      if (now_ms >= last_activity_ms_ && (now_ms - last_activity_ms_) >= dim_ms) {
        backlight_.dim();
        state_.timelapse_dimmed = backlight_.is_dimmed();
      }
    }
  }

  if (state_.modal == OverlayModal::TimelapseCompiling) {
    state_.timelapse = timelapse_engine_.state();
    finish_compile_if_ready(now_ms);
  }

  if (state_.modal == OverlayModal::Gallery) {
    PreviewFrame vf;
    if (video_player_.poll_frame(vf)) {
      gallery_frame_ = std::move(vf);
      has_gallery_preview_ = true;
    }
    state_.video_player = video_player_.state();
  }

  sync_backend_status();
}

void UiController::switch_mode(AppMode target) {
  if (state_.mode == target) {
    return;
  }
  state_.timer_active = false;
  state_.timer_remaining_sec = 0;
  state_.mode = target;
  state_.rail_cursor = 1;
  std::string err;
  reconfigure_backend(err);
}

void UiController::adjust_zoom(int delta_step) {
  int cur_idx = 0;
  float best_diff = 1e9f;
  for (size_t i = 0; i < kZoomSteps.size(); ++i) {
    const float d = std::abs(state_.zoom - kZoomSteps[i]);
    if (d < best_diff) {
      best_diff = d;
      cur_idx = static_cast<int>(i);
    }
  }
  const int next_idx =
      std::clamp(cur_idx + delta_step, 0, static_cast<int>(kZoomSteps.size()) - 1);
  if (next_idx == cur_idx && state_.zoom == kZoomSteps[static_cast<size_t>(cur_idx)]) {
    return;
  }
  state_.zoom = kZoomSteps[static_cast<size_t>(next_idx)];
  if (backend_) {
    std::string err;
    backend_->set_controls(state_.zoom, static_cast<float>(state_.config.ev), err);
  }
}

void UiController::adjust_ev(int delta_step) {
  double next_ev = state_.config.ev + static_cast<double>(delta_step) * 0.5;
  next_ev = std::clamp(std::round(next_ev * 2.0) / 2.0, -2.0, 2.0);
  if (std::abs(next_ev - state_.config.ev) < 1e-6) {
    return;
  }
  state_.config.ev = next_ev;
  if (backend_) {
    std::string err;
    backend_->set_controls(state_.zoom, static_cast<float>(state_.config.ev), err);
  }
  persist_config();
}

void UiController::adjust_rail_setting(int delta) {
  const int step = (delta >= 0) ? 1 : -1;
  switch (state_.mode) {
    case AppMode::Photo: {
      if (state_.rail_cursor == 0) {
        state_.config.photo_resolution =
            (state_.config.photo_resolution == "8mp") ? "2mp" : "8mp";
        std::string err;
        reconfigure_backend(err);
        persist_config();
      } else if (state_.rail_cursor == 1) {
        adjust_ev(step);
      } else if (state_.rail_cursor == 2) {
        state_.filter = next_filter(state_.filter, step);
        state_.config.photo_filter = filter_to_string(state_.filter);
        persist_config();
      } else if (state_.rail_cursor == 3) {
        static constexpr std::array<int, 3> kTimers = {0, 3, 10};
        const int idx = find_index_int(kTimers, state_.config.photo_timer_sec);
        state_.config.photo_timer_sec =
            kTimers[static_cast<size_t>((idx + step + 3) % 3)];
        persist_config();
      }
      break;
    }
    case AppMode::Video: {
      if (state_.rail_cursor == 0 && !state_.video_recording) {
        static constexpr std::array<const char*, 3> kVidRes = {"480p", "720p",
                                                               "1080p"};
        const int idx =
            find_index_str(kVidRes, state_.config.video_resolution);
        state_.config.video_resolution =
            kVidRes[static_cast<size_t>((idx + step + 3) % 3)];
        std::string err;
        reconfigure_backend(err);
        persist_config();
      } else if (state_.rail_cursor == 1 && !state_.video_recording) {
        state_.config.video_audio = !state_.config.video_audio;
        persist_config();
      } else if (state_.rail_cursor == 2) {
        adjust_zoom(step);
      }
      // Card 3 (FREE) is a read-only storage indicator.
      break;
    }
    case AppMode::Timelapse: {
      if (state_.timelapse.running) {
        return;
      }
      if (state_.rail_cursor == 0) {
        int idx = 0;
        int best = 1000000;
        for (size_t i = 0; i < kIntervalPresets.size(); ++i) {
          const int d =
              std::abs(state_.config.timelapse_interval_sec - kIntervalPresets[i]);
          if (d < best) {
            best = d;
            idx = static_cast<int>(i);
          }
        }
        const int next =
            (idx + step + static_cast<int>(kIntervalPresets.size())) %
            static_cast<int>(kIntervalPresets.size());
        state_.config.timelapse_interval_sec =
            kIntervalPresets[static_cast<size_t>(next)];
        persist_config();
      } else if (state_.rail_cursor == 1) {
        state_.config.timelapse_output_mode =
            (state_.config.timelapse_output_mode == "video") ? "photos"
                                                             : "video";
        persist_config();
      } else if (state_.rail_cursor == 2) {
        static constexpr std::array<const char*, 3> kTlRes = {"2mp", "1080p",
                                                              "8mp"};
        const int idx =
            find_index_str(kTlRes, state_.config.timelapse_resolution);
        state_.config.timelapse_resolution =
            kTlRes[static_cast<size_t>((idx + step + 3) % 3)];
        std::string err;
        reconfigure_backend(err);
        persist_config();
      } else if (state_.rail_cursor == 3) {
        int idx = 0;
        for (size_t i = 0; i < kStopPresets.size(); ++i) {
          if (state_.config.timelapse_stop_mode == kStopPresets[i].mode &&
              state_.config.timelapse_stop_value == kStopPresets[i].value) {
            idx = static_cast<int>(i);
            break;
          }
        }
        const int next = (idx + step + static_cast<int>(kStopPresets.size())) %
                         static_cast<int>(kStopPresets.size());
        state_.config.timelapse_stop_mode =
            kStopPresets[static_cast<size_t>(next)].mode;
        state_.config.timelapse_stop_value =
            kStopPresets[static_cast<size_t>(next)].value;
        persist_config();
      }
      break;
    }
  }
}

void UiController::trigger_photo_shutter(uint64_t now_ms) {
  if (state_.timer_active) {
    return;
  }
  if (state_.config.photo_timer_sec > 0) {
    state_.timer_active = true;
    state_.timer_remaining_sec = state_.config.photo_timer_sec;
    timer_target_ms_ =
        now_ms + static_cast<uint64_t>(state_.config.photo_timer_sec) * 1000ULL;
    return;
  }
  capture_photo_now(now_ms);
}

void UiController::capture_photo_now(uint64_t now_ms) {
  if (!backend_ || !state_.backend_ready || state_.backend_paused) {
    set_toast("Camera not ready", true, now_ms);
    return;
  }
  if (!mkdir_p(state_.effective_output_dir)) {
    set_toast("Output dir unavailable", true, now_ms);
    return;
  }
  if (!has_sufficient_storage(state_.effective_output_dir, kMinStorageBytes,
                              &state_.free_bytes)) {
    set_toast("Low Storage (<100MB)", true, now_ms);
    return;
  }

  int w = 3280;
  int h = 2464;
  photo_dims(state_.config.photo_resolution, w, h);
  const StagedPaths paths = next_photo_paths(state_.effective_output_dir);

  const StillCaptureResult cap = backend_->capture_still(paths.tmp_path, w, h);
  if (!cap.ok) {
    remove_silent(paths.tmp_path);
    set_toast(cap.error.empty() ? "Capture failed" : cap.error, true, now_ms);
    sync_backend_status();
    return;
  }

  std::string err;
  if (!finalize_still_photo(paths.tmp_path, paths.final_path, state_.filter,
                            err)) {
    remove_silent(paths.tmp_path);
    set_toast(err.empty() ? "Save failed" : err, true, now_ms);
    return;
  }

  capture_flash_pending_ = true;
  state_.capture_flash_active = true;
  flash_until_ms_ = now_ms + kCaptureFlashMs;
  refresh_storage_stats();
  set_toast("Saved " + base_name(paths.final_path), false, now_ms);
}

void UiController::start_video_recording(uint64_t now_ms) {
  if (state_.video_recording) {
    return;
  }
  if (!backend_ || !state_.backend_ready || state_.backend_paused) {
    set_toast("Camera not ready", true, now_ms);
    return;
  }
  if (!mkdir_p(state_.effective_output_dir)) {
    set_toast("Output dir unavailable", true, now_ms);
    return;
  }
  if (!has_sufficient_storage(state_.effective_output_dir, kMinStorageBytes,
                              &state_.free_bytes)) {
    set_toast("Low Storage (<100MB)", true, now_ms);
    return;
  }

  const StagedPaths paths = next_video_paths(state_.effective_output_dir);
  VideoSpec spec;
  video_params(state_.config.video_resolution, spec.width, spec.height,
               spec.bitrate_bps);
  spec.fps = 30;
  spec.audio_enabled = state_.config.video_audio;
  spec.tmp_path = paths.tmp_path;

  std::string err;
  if (!backend_->start_video(spec, err)) {
    remove_silent(paths.tmp_path);
    set_toast(err.empty() ? "Video start failed" : err, true, now_ms);
    sync_backend_status();
    return;
  }

  state_.video_recording = true;
  state_.video_elapsed_sec = 0;
  video_start_ms_ = now_ms;
  last_storage_check_ms_ = now_ms;
  video_tmp_path_ = paths.tmp_path;
  video_final_path_ = paths.final_path;
  warned_audio_ = false;
  sync_backend_status();
}

void UiController::stop_video_recording(bool low_storage, uint64_t now_ms) {
  if (!state_.video_recording) {
    return;
  }
  const VideoStopResult res = backend_->stop_video();
  state_.video_recording = false;
  state_.video_elapsed_sec = 0;

  std::string err;
  if (!res.ok) {
    remove_silent(video_tmp_path_);
    set_toast(res.error.empty() ? "Video stop failed" : res.error, true, now_ms);
  } else if (!commit_staged_file(video_tmp_path_, video_final_path_, err)) {
    remove_silent(video_tmp_path_);
    set_toast(err.empty() ? "Video commit failed" : err, true, now_ms);
  } else {
    set_toast(low_storage ? "Low Storage - Saved"
                          : ("Saved " + base_name(video_final_path_)),
              low_storage, now_ms);
  }

  video_tmp_path_.clear();
  video_final_path_.clear();
  refresh_storage_stats();
  sync_backend_status();
}

void UiController::start_timelapse(uint64_t now_ms) {
  if (timelapse_engine_.state().running || compile_in_flight_.load()) {
    return;
  }
  if (!timelapse_engine_.start(effective_engine_config(), now_ms)) {
    set_toast(timelapse_engine_.state().last_error, true, now_ms);
    return;
  }
  state_.timelapse = timelapse_engine_.state();
  last_activity_ms_ = now_ms;
}

void UiController::open_gallery(uint64_t now_ms) {
  std::string err;
  if (backend_ && !state_.backend_paused) {
    backend_->pause(err);
    sync_backend_status();
  }
  gallery_store_.refresh(state_.effective_output_dir);
  state_.gallery_items = gallery_store_.items();
  state_.gallery_in_session = gallery_store_.in_session();
  state_.gallery_session_name = gallery_store_.session_name();
  state_.gallery_cursor = 0;
  state_.gallery_delete_confirm = false;
  state_.modal = OverlayModal::Gallery;
  load_current_gallery_preview();
  if (state_.gallery_items.empty() && !err.empty()) {
    set_toast(err, true, now_ms);
  }
}

void UiController::close_gallery(bool resume_camera) {
  video_player_.stop();
  state_.video_player = video_player_.state();
  gallery_frame_ = PreviewFrame{};
  has_gallery_preview_ = false;
  state_.gallery_delete_confirm = false;
  state_.modal = OverlayModal::None;
  if (resume_camera && backend_) {
    std::string err;
    backend_->resume(err);
    sync_backend_status();
  }
}

void UiController::load_current_gallery_preview() {
  video_player_.stop();
  state_.video_player = video_player_.state();
  gallery_frame_ = PreviewFrame{};
  has_gallery_preview_ = false;

  if (state_.gallery_items.empty()) {
    state_.gallery_cursor = 0;
    return;
  }
  state_.gallery_cursor = std::clamp(
      state_.gallery_cursor, 0, static_cast<int>(state_.gallery_items.size()) - 1);

  RgbImage img;
  std::string err;
  if (gallery_store_.load_preview_rgb24(state_.gallery_cursor, 320, 128, img,
                                        err)) {
    gallery_frame_.seq = 1;
    gallery_frame_.width = img.width;
    gallery_frame_.height = img.height;
    gallery_frame_.rgb24 = std::move(img.rgb24);
    has_gallery_preview_ = true;
  }
  // Sync metadata populated by load_preview_rgb24 (dimensions / video duration)
  state_.gallery_items = gallery_store_.items();

  const auto& item =
      state_.gallery_items[static_cast<size_t>(state_.gallery_cursor)];
  if (item.type == GalleryItemType::Video) {
    video_player_.open(item.abs_path, 320, 128);
    if (gallery_muted_) {
      video_player_.toggle_mute();
    }
    state_.video_player = video_player_.state();
  }
}

void UiController::open_settings() {
  state_.modal = OverlayModal::Settings;
  state_.settings_cursor = 0;
  state_.modal_error.clear();
}

void UiController::apply_settings_row(int delta) {
  const int step = (delta >= 0) ? 1 : -1;
  switch (state_.settings_cursor) {
    case 0:
      if (delta >= 0) {
        open_dir_picker();
      }
      break;
    case 1:
      state_.config.rail_side =
          (state_.config.rail_side == "right") ? "left" : "right";
      persist_config();
      break;
    case 2:
      state_.theme = toggle_theme(state_.theme);
      state_.config.theme = theme_to_string(state_.theme);
      persist_config();
      break;
    case 3:
      state_.config.grid_enabled = !state_.config.grid_enabled;
      persist_config();
      break;
    case 4: {
      const int idx = find_index_int(kDimPresets, state_.config.timelapse_dim_sec);
      const int next =
          (idx + step + static_cast<int>(kDimPresets.size())) %
          static_cast<int>(kDimPresets.size());
      state_.config.timelapse_dim_sec = kDimPresets[static_cast<size_t>(next)];
      persist_config();
      break;
    }
    case 5: {
      const int idx =
          find_index_str(kOrientPresets, state_.config.camera_transform);
      const int next =
          (idx + step + static_cast<int>(kOrientPresets.size())) %
          static_cast<int>(kOrientPresets.size());
      state_.config.camera_transform =
          kOrientPresets[static_cast<size_t>(next)];
      std::string err;
      reconfigure_backend(err);
      persist_config();
      break;
    }
    default:
      break;
  }
}

void UiController::open_dir_picker() {
  std::string start = "/";
  if (is_valid_dir(state_.effective_output_dir)) {
    start = state_.effective_output_dir;
  } else {
    const char* home = std::getenv("HOME");
    if (home != nullptr && is_valid_dir(home)) {
      start = home;
    }
  }
  state_.modal = OverlayModal::DirPicker;
  state_.modal_error.clear();
  load_directory(start);
}

void UiController::load_directory(const std::string& path,
                                  const std::string& select_name) {
  std::error_code ec;
  fs::path resolved = fs::weakly_canonical(fs::path(path), ec);
  if (ec || resolved.empty()) {
    resolved = fs::path(path);
  }

  state_.dir_current_path = resolved.string();
  state_.dir_entries.clear();

  for (fs::directory_iterator
           it(resolved, fs::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    const auto& entry = *it;
    std::error_code entry_ec;
    if (!entry.is_directory(entry_ec) || entry_ec) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.empty() || name[0] == '.') {
      continue;
    }
    state_.dir_entries.push_back(entry.path().string());
  }

  std::sort(state_.dir_entries.begin(), state_.dir_entries.end(),
            [](const std::string& a, const std::string& b) {
              const std::string na = fs::path(a).filename().string();
              const std::string nb = fs::path(b).filename().string();
              const std::string la = to_lower_ascii(na);
              const std::string lb = to_lower_ascii(nb);
              if (la != lb) return la < lb;
              return na < nb;
            });

  if (state_.dir_entries.empty()) {
    state_.dir_cursor = -1;
    return;
  }

  state_.dir_cursor = 0;
  if (!select_name.empty()) {
    for (size_t i = 0; i < state_.dir_entries.size(); ++i) {
      if (fs::path(state_.dir_entries[i]).filename().string() == select_name) {
        state_.dir_cursor = static_cast<int>(i);
        break;
      }
    }
  }
}

void UiController::open_text_input() {
  state_.modal = OverlayModal::TextInput;
  state_.text_buffer.clear();
  state_.modal_error.clear();
}

void UiController::open_timelapse_custom_input() {
  state_.modal = OverlayModal::TimelapseCustomInput;
  state_.tl_custom_cursor = 0;
  state_.tl_custom_interval_sec = state_.config.timelapse_interval_sec;
  state_.tl_custom_fps = std::clamp(state_.config.timelapse_fps, 1, 30);
  state_.tl_custom_stop_mode = state_.config.timelapse_stop_mode;
  state_.tl_custom_stop_value = state_.config.timelapse_stop_value;
  state_.tl_custom_text.clear();
  state_.modal_error.clear();
}

bool UiController::handle_key(char ch, KeyCode code, uint64_t now_ms) {
  if (!initialized_) {
    init(now_ms);
  }

  last_now_ms_ = now_ms;
  last_activity_ms_ = now_ms;

  // Safe Wake Rule (Review Focus #2): any key while dimmed ONLY wakes backlight.
  if (backlight_.is_dimmed()) {
    backlight_.restore();
    state_.timelapse_dimmed = backlight_.is_dimmed();
    return true;
  }

  if (state_.modal == OverlayModal::TextInput) {
    return handle_text_input_key(ch, code, now_ms);
  }
  if (state_.modal == OverlayModal::TimelapseCustomInput) {
    return handle_tl_custom_key(ch, code, now_ms);
  }

  const KeyCode norm = normalize_key(ch, code);
  switch (state_.modal) {
    case OverlayModal::None:
      return handle_none_modal_key(ch, norm, now_ms);
    case OverlayModal::Gallery:
      return handle_gallery_key(ch, norm, now_ms);
    case OverlayModal::Settings:
      return handle_settings_key(ch, norm, now_ms);
    case OverlayModal::DirPicker:
      return handle_dir_picker_key(ch, norm, now_ms);
    case OverlayModal::TimelapseRecovery:
      return handle_tl_recovery_key(ch, norm, now_ms);
    case OverlayModal::TimelapseStopConfirm:
      return handle_tl_stop_confirm_key(ch, norm, now_ms);
    case OverlayModal::TimelapseCompiling:
      return handle_tl_compiling_key(ch, norm, now_ms);
    case OverlayModal::TextInput:
    case OverlayModal::TimelapseCustomInput:
      break;
  }
  return false;
}

bool UiController::handle_none_modal_key(char ch,
                                         KeyCode code,
                                         uint64_t now_ms) {
  const bool tl_running = state_.timelapse.running;
  const bool vid_recording = state_.video_recording;
  const char lower =
      (code == KeyCode::Char)
          ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
          : 0;

  // Active Timelapse running locks all mode/settings switches while keeping zoom/loupe/grid active
  if (tl_running) {
    if (code == KeyCode::Enter || code == KeyCode::Esc || lower == '6' ||
        lower == 'q') {
      state_.modal = OverlayModal::TimelapseStopConfirm;
      return true;
    }
    if (lower == '5') {
      adjust_zoom(-1);
      return true;
    }
    if (lower == '7') {
      adjust_zoom(+1);
      return true;
    }
    if (lower == 'z') {
      state_.loupe_enabled = !state_.loupe_enabled;
      return true;
    }
    if (lower == 'g') {
      state_.config.grid_enabled = !state_.config.grid_enabled;
      persist_config();
      return true;
    }
    return false;
  }

  // Active Video recording locks transitions while keeping zoom/loupe/grid/stop active
  if (vid_recording) {
    if (code == KeyCode::Enter || code == KeyCode::Space ||
        code == KeyCode::Esc || lower == '6') {
      stop_video_recording(false, now_ms);
      return true;
    }
    if (lower == '5') {
      adjust_zoom(-1);
      return true;
    }
    if (lower == '7') {
      adjust_zoom(+1);
      return true;
    }
    if (lower == 'z') {
      state_.loupe_enabled = !state_.loupe_enabled;
      return true;
    }
    if (lower == 'g') {
      state_.config.grid_enabled = !state_.config.grid_enabled;
      persist_config();
      return true;
    }
    return false;
  }

  // Photo countdown active: Esc cancels countdown without quitting
  if (state_.timer_active) {
    if (code == KeyCode::Esc) {
      state_.timer_active = false;
      state_.timer_remaining_sec = 0;
      set_toast("Timer cancelled", false, now_ms);
      return true;
    }
    return false;
  }

  // Idle global actions across Photo / Video / Timelapse
  if (code == KeyCode::Esc || lower == 'q') {
    quit_ = true;
    return true;
  }

  if (code == KeyCode::Enter || code == KeyCode::Space || lower == '6') {
    switch (state_.mode) {
      case AppMode::Photo:
        trigger_photo_shutter(now_ms);
        break;
      case AppMode::Video:
        start_video_recording(now_ms);
        break;
      case AppMode::Timelapse:
        start_timelapse(now_ms);
        break;
    }
    return true;
  }

  if (code == KeyCode::Up || lower == 'k') {
    if (state_.rail_cursor > 0) --state_.rail_cursor;
    return true;
  }
  if (code == KeyCode::Down || lower == 'j') {
    if (state_.rail_cursor < 3) ++state_.rail_cursor;
    return true;
  }
  if (code == KeyCode::Left || lower == 'h') {
    adjust_rail_setting(-1);
    return true;
  }
  if (code == KeyCode::Right || lower == 'l') {
    adjust_rail_setting(+1);
    return true;
  }

  if (code != KeyCode::Char) {
    return false;
  }

  switch (lower) {
    case '1':
      switch_mode(AppMode::Photo);
      return true;
    case '2':
      switch_mode(AppMode::Video);
      return true;
    case '3':
      switch_mode(AppMode::Timelapse);
      return true;
    case '5':
      adjust_zoom(-1);
      return true;
    case '7':
      adjust_zoom(+1);
      return true;
    case 'z':
      state_.loupe_enabled = !state_.loupe_enabled;
      return true;
    case 'f':
      if (state_.mode == AppMode::Photo) {
        state_.filter = next_filter(state_.filter, +1);
        state_.config.photo_filter = filter_to_string(state_.filter);
        persist_config();
        return true;
      }
      return false;
    case 'g':
      state_.config.grid_enabled = !state_.config.grid_enabled;
      persist_config();
      return true;
    case 'r':
      if (!state_.backend_ready && backend_) {
        std::string err;
        if (backend_->start(build_stream_spec(), err)) {
          set_toast("Camera recovered", false, now_ms);
        } else {
          set_toast(err.empty() ? "Retry failed" : err, true, now_ms);
        }
        sync_backend_status();
        return true;
      }
      if (state_.mode == AppMode::Photo) {
        state_.config.photo_resolution =
            (state_.config.photo_resolution == "8mp") ? "2mp" : "8mp";
      } else if (state_.mode == AppMode::Video) {
        if (state_.config.video_resolution == "720p") {
          state_.config.video_resolution = "1080p";
        } else if (state_.config.video_resolution == "1080p") {
          state_.config.video_resolution = "480p";
        } else {
          state_.config.video_resolution = "720p";
        }
      } else {
        if (state_.config.timelapse_resolution == "2mp") {
          state_.config.timelapse_resolution = "1080p";
        } else if (state_.config.timelapse_resolution == "1080p") {
          state_.config.timelapse_resolution = "8mp";
        } else {
          state_.config.timelapse_resolution = "2mp";
        }
      }
      {
        std::string err;
        reconfigure_backend(err);
      }
      persist_config();
      return true;
    case 'c':
      if (state_.mode == AppMode::Photo) {
        if (state_.config.photo_timer_sec == 0) {
          state_.config.photo_timer_sec = 3;
        } else if (state_.config.photo_timer_sec == 3) {
          state_.config.photo_timer_sec = 10;
        } else {
          state_.config.photo_timer_sec = 0;
        }
        persist_config();
        return true;
      }
      if (state_.mode == AppMode::Timelapse) {
        open_timelapse_custom_input();
        return true;
      }
      return false;
    case 'p':
      if (state_.mode == AppMode::Timelapse) {
        open_timelapse_custom_input();
        return true;
      }
      return false;
    case 'm':
      if (state_.mode == AppMode::Video) {
        state_.config.video_audio = !state_.config.video_audio;
        persist_config();
        return true;
      }
      return false;
    case 'w':
      state_.config.rail_side =
          (state_.config.rail_side == "right") ? "left" : "right";
      persist_config();
      return true;
    case 't':
      state_.theme = toggle_theme(state_.theme);
      state_.config.theme = theme_to_string(state_.theme);
      persist_config();
      return true;
    case '8':
      open_gallery(now_ms);
      return true;
    case 's':
      open_settings();
      return true;
    default:
      return false;
  }
}

bool UiController::handle_gallery_key(char ch, KeyCode code, uint64_t now_ms) {
  const char lower =
      (code == KeyCode::Char)
          ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
          : 0;

  if (state_.gallery_delete_confirm) {
    if (lower == 'y') {
      if (!state_.gallery_items.empty()) {
        const std::string deleted_name =
            state_.gallery_items[static_cast<size_t>(state_.gallery_cursor)]
                .filename;
        video_player_.stop();
        std::string err;
        if (gallery_store_.delete_item(state_.gallery_cursor, err)) {
          state_.gallery_items = gallery_store_.items();
          if (state_.gallery_cursor >=
              static_cast<int>(state_.gallery_items.size())) {
            state_.gallery_cursor =
                std::max(0, static_cast<int>(state_.gallery_items.size()) - 1);
          }
          state_.gallery_delete_confirm = false;
          load_current_gallery_preview();
          refresh_storage_stats();
          set_toast("Deleted " + deleted_name, false, now_ms);
        } else {
          state_.gallery_delete_confirm = false;
          set_toast(err.empty() ? "Delete failed" : err, true, now_ms);
        }
      } else {
        state_.gallery_delete_confirm = false;
      }
      return true;
    }
    if (lower == 'n' || code == KeyCode::Esc) {
      state_.gallery_delete_confirm = false;
      return true;
    }
    return true;
  }

  if (lower == 'q') {
    close_gallery(true);
    return true;
  }
  if (code == KeyCode::Esc) {
    if (gallery_store_.in_session()) {
      video_player_.stop();
      gallery_store_.exit_session();
      state_.gallery_items = gallery_store_.items();
      state_.gallery_in_session = gallery_store_.in_session();
      state_.gallery_session_name = gallery_store_.session_name();
      state_.gallery_cursor = 0;
      load_current_gallery_preview();
      return true;
    }
    close_gallery(true);
    return true;
  }
  if (code == KeyCode::Backspace) {
    if (gallery_store_.in_session()) {
      video_player_.stop();
      gallery_store_.exit_session();
      state_.gallery_items = gallery_store_.items();
      state_.gallery_in_session = gallery_store_.in_session();
      state_.gallery_session_name = gallery_store_.session_name();
      state_.gallery_cursor = 0;
      load_current_gallery_preview();
      return true;
    }
    return false;
  }

  if (lower == '1' || lower == '2' || lower == '3') {
    close_gallery(true);
    if (lower == '1') switch_mode(AppMode::Photo);
    if (lower == '2') switch_mode(AppMode::Video);
    if (lower == '3') switch_mode(AppMode::Timelapse);
    return true;
  }

  if (lower == 't') {
    state_.theme = toggle_theme(state_.theme);
    state_.config.theme = theme_to_string(state_.theme);
    persist_config();
    return true;
  }

  if (state_.gallery_items.empty()) {
    return false;
  }

  const auto& cur_item =
      state_.gallery_items[static_cast<size_t>(state_.gallery_cursor)];

  if (code == KeyCode::Left || lower == 'h') {
    if (cur_item.type == GalleryItemType::Video &&
        video_player_.state().playing) {
      video_player_.seek_delta(-5.0);
      state_.video_player = video_player_.state();
      return true;
    }
    if (state_.gallery_cursor > 0) {
      --state_.gallery_cursor;
      load_current_gallery_preview();
    }
    return true;
  }

  if (code == KeyCode::Right || lower == 'l') {
    if (cur_item.type == GalleryItemType::Video &&
        video_player_.state().playing) {
      video_player_.seek_delta(+5.0);
      state_.video_player = video_player_.state();
      return true;
    }
    if (state_.gallery_cursor + 1 <
        static_cast<int>(state_.gallery_items.size())) {
      ++state_.gallery_cursor;
      load_current_gallery_preview();
    }
    return true;
  }

  if (code == KeyCode::Up || lower == 'k') {
    if (state_.gallery_cursor > 0) {
      --state_.gallery_cursor;
      load_current_gallery_preview();
    }
    return true;
  }
  if (code == KeyCode::Down || lower == 'j') {
    if (state_.gallery_cursor + 1 <
        static_cast<int>(state_.gallery_items.size())) {
      ++state_.gallery_cursor;
      load_current_gallery_preview();
    }
    return true;
  }

  if (code == KeyCode::Space || code == KeyCode::Enter) {
    if (cur_item.type == GalleryItemType::TimelapseFolder) {
      if (gallery_store_.enter_session(state_.gallery_cursor)) {
        state_.gallery_items = gallery_store_.items();
        state_.gallery_in_session = gallery_store_.in_session();
        state_.gallery_session_name = gallery_store_.session_name();
        state_.gallery_cursor = 0;
        load_current_gallery_preview();
      }
      return true;
    }
    if (cur_item.type == GalleryItemType::Video) {
      const VideoPlayerState vst = video_player_.state();
      if (vst.playing) {
        video_player_.pause();
      } else {
        video_player_.play(!gallery_muted_);
      }
      state_.video_player = video_player_.state();
      return true;
    }
    return false;
  }

  if (lower == 'm' && cur_item.type == GalleryItemType::Video) {
    gallery_muted_ = !gallery_muted_;
    video_player_.toggle_mute();
    state_.video_player = video_player_.state();
    return true;
  }

  if (code == KeyCode::Delete || lower == 'd') {
    if (video_player_.state().playing) {
      video_player_.pause();
      state_.video_player = video_player_.state();
    }
    state_.gallery_delete_confirm = true;
    return true;
  }

  return false;
}

bool UiController::handle_settings_key(char ch, KeyCode code, uint64_t /*now_ms*/) {
  const char lower =
      (code == KeyCode::Char)
          ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
          : 0;

  if (code == KeyCode::Esc) {
    persist_config();
    state_.modal = OverlayModal::None;
    return true;
  }
  if (code == KeyCode::Up || lower == 'k') {
    if (state_.settings_cursor > 0) --state_.settings_cursor;
    return true;
  }
  if (code == KeyCode::Down || lower == 'j') {
    if (state_.settings_cursor < 5) ++state_.settings_cursor;
    return true;
  }
  if (code == KeyCode::Left || lower == 'h') {
    apply_settings_row(-1);
    return true;
  }
  if (code == KeyCode::Right || lower == 'l' || code == KeyCode::Enter ||
      code == KeyCode::Space) {
    apply_settings_row(+1);
    return true;
  }
  return false;
}

bool UiController::handle_dir_picker_key(char ch,
                                         KeyCode code,
                                         uint64_t now_ms) {
  const char lower =
      (code == KeyCode::Char)
          ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
          : 0;

  if (code == KeyCode::Esc) {
    state_.modal = OverlayModal::Settings;
    return true;
  }
  if (code == KeyCode::Up || lower == 'k') {
    if (state_.dir_cursor > 0) --state_.dir_cursor;
    return true;
  }
  if (code == KeyCode::Down || lower == 'j') {
    const int max_idx = static_cast<int>(state_.dir_entries.size()) - 1;
    if (state_.dir_cursor < max_idx) ++state_.dir_cursor;
    return true;
  }
  if (code == KeyCode::Enter) {
    if (state_.dir_cursor >= 0 &&
        state_.dir_cursor < static_cast<int>(state_.dir_entries.size())) {
      load_directory(state_.dir_entries[static_cast<size_t>(state_.dir_cursor)]);
    }
    return true;
  }
  if (code == KeyCode::Backspace) {
    const fs::path cur(state_.dir_current_path);
    const fs::path parent = cur.parent_path();
    if (!parent.empty() && parent != cur) {
      load_directory(parent.string(), cur.filename().string());
    }
    return true;
  }
  if (code == KeyCode::Space || ch == '.') {
    std::string chosen = state_.dir_current_path;
    if (code == KeyCode::Space && state_.dir_cursor >= 0 &&
        state_.dir_cursor < static_cast<int>(state_.dir_entries.size())) {
      chosen = state_.dir_entries[static_cast<size_t>(state_.dir_cursor)];
    }
    if (!chosen.empty()) {
      if (!mkdir_p(chosen)) {
        set_toast("Cannot access folder", true, now_ms);
        return true;
      }
      state_.config.output_dir = chosen;
      state_.effective_output_dir = expand_user_path(chosen);
      output_dir_override_active_ = false;
      persist_config();
      refresh_storage_stats();
      state_.modal = OverlayModal::Settings;
      set_toast("Output: " + base_name(chosen), false, now_ms);
    }
    return true;
  }
  if (lower == 't') {
    open_text_input();
    return true;
  }
  if (ch == '/') {
    load_directory("/");
    return true;
  }
  if (ch == '~') {
    const char* home = std::getenv("HOME");
    if (home != nullptr && is_valid_dir(home)) {
      load_directory(home);
    }
    return true;
  }
  return false;
}

bool UiController::handle_text_input_key(char ch,
                                         KeyCode code,
                                         uint64_t now_ms) {
  if (code == KeyCode::Esc ||
      (code == KeyCode::Char && static_cast<unsigned char>(ch) == 0x1b)) {
    state_.modal_error.clear();
    state_.modal = OverlayModal::DirPicker;
    return true;
  }
  if (code == KeyCode::Backspace ||
      (code == KeyCode::Char &&
       (ch == '\b' || static_cast<unsigned char>(ch) == 0x7f))) {
    if (!state_.text_buffer.empty()) {
      state_.text_buffer.pop_back();
      state_.modal_error.clear();
    }
    return true;
  }
  if (code == KeyCode::Enter ||
      (code == KeyCode::Char && (ch == '\n' || ch == '\r'))) {
    const std::string raw = trim_ascii(state_.text_buffer);
    if (raw.empty()) {
      state_.modal_error = "Path cannot be empty";
      return true;
    }
    const std::string expanded = expand_user_path(raw);
    if (!mkdir_p(expanded)) {
      state_.modal_error = "Cannot create or access directory";
      return true;
    }
    state_.config.output_dir = raw;
    state_.effective_output_dir = expanded;
    output_dir_override_active_ = false;
    persist_config();
    refresh_storage_stats();
    state_.modal_error.clear();
    state_.modal = OverlayModal::Settings;
    set_toast("Output: " + base_name(expanded), false, now_ms);
    return true;
  }
  if (code == KeyCode::Space) {
    if (state_.text_buffer.size() < 256) {
      state_.text_buffer.push_back(' ');
      state_.modal_error.clear();
    }
    return true;
  }
  if (code == KeyCode::Char) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (uch >= 0x20 && uch <= 0x7e && state_.text_buffer.size() < 256) {
      state_.text_buffer.push_back(ch);
      state_.modal_error.clear();
    }
    return true;
  }
  return false;
}

bool UiController::handle_tl_custom_key(char ch,
                                        KeyCode code,
                                        uint64_t /*now_ms*/) {
  if (code == KeyCode::Esc ||
      (code == KeyCode::Char && static_cast<unsigned char>(ch) == 0x1b)) {
    state_.modal_error.clear();
    state_.tl_custom_text.clear();
    state_.modal = OverlayModal::None;
    return true;
  }

  if (code == KeyCode::Enter ||
      (code == KeyCode::Char && (ch == '\n' || ch == '\r'))) {
    state_.config.timelapse_interval_sec =
        std::clamp(state_.tl_custom_interval_sec, 1, 3600);
    state_.config.timelapse_fps = std::clamp(state_.tl_custom_fps, 1, 30);
    state_.config.timelapse_stop_mode = state_.tl_custom_stop_mode;
    state_.config.timelapse_stop_value =
        (state_.tl_custom_stop_mode == "manual")
            ? 0
            : std::max(1, state_.tl_custom_stop_value);
    persist_config();
    state_.tl_custom_text.clear();
    state_.modal = OverlayModal::None;
    return true;
  }

  const char lower =
      (code == KeyCode::Char)
          ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
          : 0;

  if (code == KeyCode::Up || lower == 'k') {
    if (state_.tl_custom_cursor > 0) {
      --state_.tl_custom_cursor;
      state_.tl_custom_text.clear();
    }
    return true;
  }
  if (code == KeyCode::Down || lower == 'j') {
    if (state_.tl_custom_cursor < 3) {
      ++state_.tl_custom_cursor;
      state_.tl_custom_text.clear();
    }
    return true;
  }

  // Numeric entry for Interval (row 0), Video FPS (row 1), or Stop Value (row 3)
  if (code == KeyCode::Char && std::isdigit(static_cast<unsigned char>(ch))) {
    if (state_.tl_custom_cursor == 0 || state_.tl_custom_cursor == 1 ||
        (state_.tl_custom_cursor == 3 &&
         state_.tl_custom_stop_mode != "manual")) {
      const size_t max_len = (state_.tl_custom_cursor == 1) ? 2 : 4;
      if (state_.tl_custom_text.size() < max_len) {
        state_.tl_custom_text.push_back(ch);
        const int val = std::atoi(state_.tl_custom_text.c_str());
        if (state_.tl_custom_cursor == 0) {
          state_.tl_custom_interval_sec = std::clamp(val, 1, 3600);
        } else if (state_.tl_custom_cursor == 1) {
          state_.tl_custom_fps = std::clamp(val, 1, 30);
        } else if (state_.tl_custom_stop_mode == "shots") {
          state_.tl_custom_stop_value = std::clamp(val, 1, 9999);
        } else {
          state_.tl_custom_stop_value = std::clamp(val, 1, 1440) * 60;
        }
      }
      return true;
    }
  }

  if (code == KeyCode::Backspace ||
      (code == KeyCode::Char &&
       (ch == '\b' || static_cast<unsigned char>(ch) == 0x7f))) {
    if (state_.tl_custom_cursor == 1 && state_.tl_custom_text.empty()) {
      state_.tl_custom_text = std::to_string(std::clamp(state_.tl_custom_fps, 1, 30));
    }
    if (!state_.tl_custom_text.empty()) {
      state_.tl_custom_text.pop_back();
      const int val =
          state_.tl_custom_text.empty() ? 1
                                        : std::atoi(state_.tl_custom_text.c_str());
      if (state_.tl_custom_cursor == 0) {
        state_.tl_custom_interval_sec = std::clamp(val, 1, 3600);
      } else if (state_.tl_custom_cursor == 1) {
        state_.tl_custom_fps = std::clamp(val, 1, 30);
      } else if (state_.tl_custom_stop_mode == "shots") {
        state_.tl_custom_stop_value = std::clamp(val, 1, 9999);
      } else if (state_.tl_custom_stop_mode == "duration") {
        state_.tl_custom_stop_value = std::clamp(val, 1, 1440) * 60;
      }
      return true;
    }
  }

  if (code == KeyCode::Left || code == KeyCode::Right || lower == 'h' ||
      lower == 'l') {
    const int delta = (code == KeyCode::Left || lower == 'h') ? -1 : +1;
    state_.tl_custom_text.clear();
    if (state_.tl_custom_cursor == 0) {
      const int step = (state_.tl_custom_interval_sec >= 20) ? 5 : 1;
      state_.tl_custom_interval_sec =
          std::clamp(state_.tl_custom_interval_sec + delta * step, 1, 3600);
    } else if (state_.tl_custom_cursor == 1) {
      state_.tl_custom_fps =
          std::clamp(state_.tl_custom_fps + delta, 1, 30);
    } else if (state_.tl_custom_cursor == 2) {
      static constexpr std::array<const char*, 3> kStopModes = {
          "manual", "shots", "duration"};
      const int idx = find_index_str(kStopModes, state_.tl_custom_stop_mode);
      state_.tl_custom_stop_mode =
          kStopModes[static_cast<size_t>((idx + delta + 3) % 3)];
      if (state_.tl_custom_stop_mode == "manual") {
        state_.tl_custom_stop_value = 0;
      } else if (state_.tl_custom_stop_mode == "shots" &&
                 state_.tl_custom_stop_value <= 0) {
        state_.tl_custom_stop_value = 60;
      } else if (state_.tl_custom_stop_mode == "duration" &&
                 state_.tl_custom_stop_value < 60) {
        state_.tl_custom_stop_value = 900;
      }
    } else if (state_.tl_custom_cursor == 3) {
      if (state_.tl_custom_stop_mode == "shots") {
        state_.tl_custom_stop_value =
            std::clamp(state_.tl_custom_stop_value + delta * 10, 10, 9999);
      } else if (state_.tl_custom_stop_mode == "duration") {
        const int mins = std::max(1, state_.tl_custom_stop_value / 60);
        state_.tl_custom_stop_value =
            std::clamp(mins + delta * 5, 1, 1440) * 60;
      }
    }
    return true;
  }

  return false;
}

bool UiController::handle_tl_recovery_key(char ch,
                                          KeyCode code,
                                          uint64_t now_ms) {
  if (code == KeyCode::Esc) {
    state_.modal = OverlayModal::None;
    return true;
  }
  if (code != KeyCode::Char) {
    return false;
  }
  const char lower =
      static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  if (lower == 'c') {
    const TimelapseManifest m = state_.recovery_manifest;
    const TimelapseSessionPaths paths = session_paths_from_manifest(m);
    const int fps = (m.fps > 0) ? m.fps : state_.config.timelapse_fps;
    const std::string res =
        m.resolution.empty() ? state_.config.timelapse_resolution : m.resolution;
    start_compile_async(paths, fps, res, m, true);
    return true;
  }
  if (lower == 'k') {
    std::string msg;
    const bool ok = timelapse_engine_.recover_session(
        state_.recovery_manifest, RecoveryAction::KeepPhotos, msg);
    set_toast(msg, !ok, now_ms);
    refresh_storage_stats();
    advance_recovery_queue();
    return true;
  }
  if (lower == 'd') {
    std::string msg;
    const bool ok = timelapse_engine_.recover_session(
        state_.recovery_manifest, RecoveryAction::Discard, msg);
    set_toast(msg, !ok, now_ms);
    refresh_storage_stats();
    advance_recovery_queue();
    return true;
  }
  return false;
}

bool UiController::handle_tl_stop_confirm_key(char ch,
                                              KeyCode code,
                                              uint64_t now_ms) {
  const char lower =
      (code == KeyCode::Char)
          ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch)))
          : 0;

  if (code == KeyCode::Esc || lower == 'n') {
    state_.modal = OverlayModal::None;
    return true;
  }

  if (lower == 'y') {
    state_.modal = OverlayModal::None;
    timelapse_engine_.request_stop();
    state_.timelapse = timelapse_engine_.state();
    refresh_storage_stats();

    if (state_.timelapse.needs_compile) {
      const TimelapseSessionPaths sp = timelapse_engine_.session_paths();
      TimelapseManifest fb;
      fb.session_id = sp.session_id;
      fb.work_dir = sp.work_dir;
      fb.resolution = state_.config.timelapse_resolution;
      fb.fps = state_.config.timelapse_fps;
      fb.frame_count = state_.timelapse.shots_taken;
      start_compile_async(sp, state_.config.timelapse_fps,
                          state_.config.timelapse_resolution, fb, false);
    } else if (!state_.timelapse.result_path.empty()) {
      set_toast("Saved " + base_name(state_.timelapse.result_path), false,
                now_ms);
    } else {
      set_toast("Timelapse stopped", false, now_ms);
    }
    return true;
  }

  return false;
}

bool UiController::handle_tl_compiling_key(char /*ch*/,
                                           KeyCode code,
                                           uint64_t now_ms) {
  if (code == KeyCode::Esc) {
    compile_cancelled_by_user_ = true;
    timelapse_engine_.cancel_compile();
    join_compile_thread();
    compile_finished_.store(true, std::memory_order_release);
    finish_compile_if_ready(now_ms);
    return true;
  }
  return false;
}

}  // namespace cz
