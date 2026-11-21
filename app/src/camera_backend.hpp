#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cz {

struct CameraStreamSpec {
  std::string mode = "photo";  // "photo" | "video" | "timelapse"
  int main_width = 3280;
  int main_height = 2464;
  int preview_width = 226;
  int preview_height = 170;
  int fps = 30;
  float zoom = 1.0f;
  float ev = 0.0f;
  std::string camera_transform = "auto";  // "auto" | "0" | "180" | "hflip" | "vflip"
};

struct PreviewFrame {
  uint64_t seq = 0;
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgb24;
  bool is_16_9 = false;
};

struct StillCaptureResult {
  bool ok = false;
  std::string error;
  int width = 0;
  int height = 0;
  uint64_t bytes = 0;
};

struct VideoSpec {
  int width = 1280;
  int height = 720;
  int fps = 30;
  int bitrate_bps = 4000000;
  bool audio_enabled = true;
  std::string tmp_path;
};

struct VideoStopResult {
  bool ok = false;
  std::string error;
  double duration_sec = 0.0;
  uint64_t bytes = 0;
  bool audio_recorded = false;
};

struct BackendHealth {
  bool ready = false;
  bool recording = false;
  bool paused = false;
  bool audio_warning = false;
  std::string last_error;
  std::string backend_name;
};

class ICameraBackend {
 public:
  virtual ~ICameraBackend() = default;

  virtual bool start(const CameraStreamSpec& initial_spec,
                     std::string& out_err) = 0;
  virtual void cancel_start() {}
  virtual void reset_start_cancel() {}
  virtual void stop() = 0;
  virtual bool reconfigure(const CameraStreamSpec& spec,
                           std::string& out_err) = 0;
  virtual bool set_controls(float zoom, float ev, std::string& out_err) = 0;
  virtual bool pause(std::string& out_err) = 0;
  virtual bool resume(std::string& out_err) = 0;
  virtual bool copy_latest_preview(uint64_t last_seen_seq,
                                   PreviewFrame& out_frame) = 0;
  virtual StillCaptureResult capture_still(const std::string& tmp_jpeg_path,
                                           int width,
                                           int height) = 0;
  virtual bool start_video(const VideoSpec& spec, std::string& out_err) = 0;
  virtual VideoStopResult stop_video() = 0;
  virtual BackendHealth status() const = 0;
};

std::unique_ptr<ICameraBackend> create_camera_backend(bool force_mock = false);

}  // namespace cz
