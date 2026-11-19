#pragma once

#include "backlight.hpp"
#include "camera_backend.hpp"
#include "config.hpp"
#include "filters.hpp"
#include "gallery_model.hpp"
#include "theme.hpp"
#include "timelapse_engine.hpp"
#include "video_player.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cz {

enum class KeyCode {
  Char,
  Enter,
  Space,
  Backspace,
  Delete,
  Esc,
  Up,
  Down,
  Left,
  Right,
};

enum class AppMode {
  Photo = 0,
  Video = 1,
  Timelapse = 2,
};

enum class OverlayModal {
  None,
  Gallery,
  Settings,
  DirPicker,
  TextInput,
  TimelapseCustomInput,
  TimelapseRecovery,
  TimelapseStopConfirm,
  TimelapseCompiling,
};

struct UiControllerOptions {
  std::string config_path;
  std::string output_dir_override;
  std::string backlight_sysfs_root = "/sys/class/backlight";
  bool mock_backend = false;
  bool persist_config = true;
  bool skip_startup_recovery_scan = false;
};

struct UiState {
  AppMode mode = AppMode::Photo;
  OverlayModal modal = OverlayModal::None;
  ThemeMode theme = ThemeMode::Dark;
  AppConfig config;
  std::string effective_output_dir;

  // Rail & camera live controls
  int rail_cursor = 1;  // 0..3
  float zoom = 1.0f;
  bool loupe_enabled = false;
  FilterId filter = FilterId::None;

  // Photo countdown & flash
  bool timer_active = false;
  int timer_remaining_sec = 0;
  bool capture_flash_active = false;

  // Video recording
  bool video_recording = false;
  int video_elapsed_sec = 0;

  // Timelapse runtime & dimming
  TimelapseState timelapse;
  bool timelapse_dimmed = false;
  bool has_hardware_backlight = false;

  // Timelapse custom input modal
  int tl_custom_cursor = 0;  // 0: interval, 1: fps, 2: stop_mode, 3: stop_value
  int tl_custom_interval_sec = 5;
  int tl_custom_fps = 24;
  std::string tl_custom_stop_mode = "manual";
  int tl_custom_stop_value = 0;
  std::string tl_custom_text;

  // Timelapse orphan recovery modal
  TimelapseManifest recovery_manifest;
  int recovery_remaining = 0;

  // Settings modal
  int settings_cursor = 0;  // 0..5

  // Directory picker & manual path input
  std::string dir_current_path;
  std::vector<std::string> dir_entries;
  int dir_cursor = -1;
  std::string text_buffer;
  std::string modal_error;

  // Gallery modal
  std::vector<GalleryItem> gallery_items;
  int gallery_cursor = 0;
  bool gallery_in_session = false;
  std::string gallery_session_name;
  bool gallery_delete_confirm = false;
  VideoPlayerState video_player;

  // Status / storage / toast
  uint64_t free_bytes = 0;
  bool backend_ready = false;
  bool backend_paused = false;
  std::string backend_error;
  std::string toast_message;
  bool toast_is_error = false;
};

class UiController {
 public:
  explicit UiController(std::unique_ptr<ICameraBackend> backend = nullptr,
                        const UiControllerOptions& opts = UiControllerOptions{});
  ~UiController();

  UiController(const UiController&) = delete;
  UiController& operator=(const UiController&) = delete;

  bool init(uint64_t now_ms);
  void shutdown();

  void tick(uint64_t now_ms);
  bool handle_key(char ch, KeyCode code, uint64_t now_ms);

  const UiState& state() const { return state_; }
  bool should_quit() const { return quit_; }
  bool consume_capture_flash();

  ICameraBackend* backend() { return backend_.get(); }
  const BacklightController& backlight() const { return backlight_; }
  BacklightController& backlight() { return backlight_; }
  VideoPlayer& video_player() { return video_player_; }
  const PreviewFrame* gallery_preview() const {
    return has_gallery_preview_ ? &gallery_frame_ : nullptr;
  }

  void set_toast(const std::string& message, bool is_error, uint64_t now_ms);
  void refresh_storage_stats();

 private:
  CameraStreamSpec build_stream_spec() const;
  AppConfig effective_engine_config() const;
  void persist_config();
  bool reconfigure_backend(std::string& out_err);
  void sync_backend_status();

  void switch_mode(AppMode target);
  void adjust_zoom(int delta_step);
  void adjust_ev(int delta_step);
  void adjust_rail_setting(int delta);
  void trigger_photo_shutter(uint64_t now_ms);
  void capture_photo_now(uint64_t now_ms);

  void start_video_recording(uint64_t now_ms);
  void stop_video_recording(bool low_storage, uint64_t now_ms);

  void start_timelapse(uint64_t now_ms);
  void start_compile_async(const TimelapseSessionPaths& paths,
                           int fps,
                           const std::string& resolution,
                           const TimelapseManifest& fallback_manifest,
                           bool from_recovery);
  void join_compile_thread();
  void finish_compile_if_ready(uint64_t now_ms);
  void advance_recovery_queue();

  void open_gallery(uint64_t now_ms);
  void close_gallery(bool resume_camera);
  void load_current_gallery_preview();

  void open_settings();
  void apply_settings_row(int delta);
  void open_dir_picker();
  void load_directory(const std::string& path, const std::string& select_name = "");
  void open_text_input();
  void open_timelapse_custom_input();

  bool handle_none_modal_key(char ch, KeyCode code, uint64_t now_ms);
  bool handle_gallery_key(char ch, KeyCode code, uint64_t now_ms);
  bool handle_settings_key(char ch, KeyCode code, uint64_t now_ms);
  bool handle_dir_picker_key(char ch, KeyCode code, uint64_t now_ms);
  bool handle_text_input_key(char ch, KeyCode code, uint64_t now_ms);
  bool handle_tl_custom_key(char ch, KeyCode code, uint64_t now_ms);
  bool handle_tl_recovery_key(char ch, KeyCode code, uint64_t now_ms);
  bool handle_tl_stop_confirm_key(char ch, KeyCode code, uint64_t now_ms);
  bool handle_tl_compiling_key(char ch, KeyCode code, uint64_t now_ms);

  UiControllerOptions opts_;
  std::unique_ptr<ICameraBackend> backend_;
  BacklightController backlight_;
  TimelapseEngine timelapse_engine_;
  GalleryStore gallery_store_;
  VideoPlayer video_player_;

  UiState state_;
  std::string persisted_output_dir_;
  bool output_dir_override_active_ = false;
  bool initialized_ = false;
  bool quit_ = false;
  bool capture_flash_pending_ = false;

  uint64_t last_now_ms_ = 0;
  uint64_t last_activity_ms_ = 0;
  uint64_t timer_target_ms_ = 0;
  uint64_t flash_until_ms_ = 0;
  uint64_t toast_until_ms_ = 0;

  uint64_t video_start_ms_ = 0;
  uint64_t last_storage_check_ms_ = 0;
  std::string video_tmp_path_;
  std::string video_final_path_;
  bool warned_audio_ = false;

  PreviewFrame gallery_frame_;
  bool has_gallery_preview_ = false;
  bool gallery_muted_ = false;

  std::vector<TimelapseManifest> orphaned_timelapses_;

  std::thread compile_thread_;
  std::atomic<bool> compile_in_flight_{false};
  std::atomic<bool> compile_finished_{false};
  std::mutex compile_result_mutex_;
  bool compile_ok_ = false;
  bool compile_cancelled_by_user_ = false;
  bool compile_from_recovery_ = false;
  std::string compile_result_msg_;
  TimelapseManifest compile_fallback_manifest_;
};

}  // namespace cz
