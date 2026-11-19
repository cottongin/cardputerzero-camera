#pragma once

#include "camera_backend.hpp"
#include "config.hpp"
#include "storage.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <vector>

namespace cz {

enum class RecoveryAction {
  Compile,
  KeepPhotos,
  Discard,
};

struct TimelapseState {
  bool running = false;
  bool compiling = false;
  int shots_taken = 0;
  std::string stop_mode = "manual";
  int stop_value = 0;
  int interval_sec = 5;
  int next_shot_in_sec = 0;
  int elapsed_sec = 0;
  int compile_current = 0;
  int compile_total = 0;
  float compile_progress = 0.0f;
  std::string last_error;
  std::string result_path;
  std::string output_mode = "video";
  bool needs_compile = false;
  bool stopped_by_low_storage = false;
};

class TimelapseEngine {
 public:
  explicit TimelapseEngine(ICameraBackend* backend = nullptr);
  ~TimelapseEngine();

  TimelapseEngine(const TimelapseEngine&) = delete;
  TimelapseEngine& operator=(const TimelapseEngine&) = delete;

  void set_backend(ICameraBackend* backend);

  bool start(const AppConfig& cfg, uint64_t now_ms);
  void tick(uint64_t now_ms, const AppConfig& cfg);
  void request_stop();

  bool compile_session(const TimelapseSessionPaths& paths,
                       int fps,
                       const std::string& resolution,
                       std::function<void(int, int)> on_progress = nullptr);
  void cancel_compile();

  bool recover_session(const TimelapseManifest& manifest,
                       RecoveryAction action,
                       std::string& out_msg);

  TimelapseState state() const;
  TimelapseSessionPaths session_paths() const;

  static std::vector<std::string> build_compile_command(
      const TimelapseSessionPaths& paths,
      int fps,
      const std::string& resolution,
      bool use_hw_encoder);

 private:
  void finish_capture_locked();
  bool run_compile_attempt(const std::string& ffmpeg_exe,
                           const TimelapseSessionPaths& paths,
                           int fps,
                           const std::string& resolution,
                           bool use_hw_encoder,
                           int total_frames,
                           const std::function<void(int, int)>& on_progress,
                           std::string& out_err);

  mutable std::mutex mutex_;
  std::mutex compile_mutex_;
  std::mutex compile_proc_mutex_;

  ICameraBackend* backend_ = nullptr;
  TimelapseState state_;
  TimelapseSessionPaths session_;
  TimelapseManifest manifest_;
  std::string output_dir_;
  std::string resolution_ = "2mp";
  uint64_t start_ms_ = 0;
  uint64_t next_shot_ms_ = 0;
  bool capturing_ = false;
  bool stop_requested_ = false;

  bool cancel_compile_requested_ = false;
  pid_t compile_pid_ = -1;
  int cancel_wake_r_ = -1;
  int cancel_wake_w_ = -1;
};

}  // namespace cz
