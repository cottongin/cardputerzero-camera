#pragma once

#include "camera_backend.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cz {

class SimCameraBackend final : public ICameraBackend {
 public:
  SimCameraBackend() = default;
  ~SimCameraBackend() override;

  SimCameraBackend(const SimCameraBackend&) = delete;
  SimCameraBackend& operator=(const SimCameraBackend&) = delete;
  SimCameraBackend(SimCameraBackend&&) = delete;
  SimCameraBackend& operator=(SimCameraBackend&&) = delete;

  bool start(const CameraStreamSpec& initial_spec,
             std::string& out_err) override;
  void stop() override;
  bool reconfigure(const CameraStreamSpec& spec,
                   std::string& out_err) override;
  bool set_controls(float zoom, float ev, std::string& out_err) override;
  bool pause(std::string& out_err) override;
  bool resume(std::string& out_err) override;
  bool copy_latest_preview(uint64_t last_seen_seq,
                           PreviewFrame& out_frame) override;
  StillCaptureResult capture_still(const std::string& tmp_jpeg_path,
                                   int width,
                                   int height) override;
  bool start_video(const VideoSpec& spec, std::string& out_err) override;
  VideoStopResult stop_video() override;
  BackendHealth status() const override;

 private:
  void render_loop();
  void publish_frame_locked_state(uint64_t tick,
                                  const CameraStreamSpec& spec,
                                  bool is_16_9);

  mutable std::mutex state_mutex_;
  std::condition_variable state_cv_;

  bool running_ = false;
  bool ready_ = false;
  bool recording_ = false;
  bool paused_ = false;
  bool stop_thread_ = false;
  std::string last_error_;

  CameraStreamSpec spec_;
  uint64_t tick_ = 0;

  VideoSpec active_video_;
  std::chrono::steady_clock::time_point video_start_time_;

  std::mutex frame_mutex_;
  PreviewFrame latest_frame_;
  uint64_t seq_counter_ = 0;

  std::thread render_thread_;
};

}  // namespace cz
