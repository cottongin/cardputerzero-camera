#pragma once

#include "camera_backend.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace cz {

inline constexpr size_t kCzfrHeaderBytes = 16;
inline constexpr uint8_t kCzfrPixfmtYuv420 = 1;
inline constexpr uint8_t kCzfrPixfmtRgb24 = 2;
inline constexpr uint8_t kCzfrFlagAspect16_9 = 0x01;
inline constexpr uint32_t kCzfrMaxPayloadBytes = 16u * 1024u * 1024u;

bool parse_czfr_header(const uint8_t* hdr16,
                       uint16_t& w,
                       uint16_t& h,
                       uint8_t& pixfmt,
                       uint8_t& flags,
                       uint32_t& payload_bytes);

bool yuv420_to_rgb24(const uint8_t* yuv,
                     int width,
                     int height,
                     std::vector<uint8_t>& out_rgb24);

namespace json_detail {
struct JsonValue;
}

class SubprocessCameraBackend final : public ICameraBackend {
 public:
  explicit SubprocessCameraBackend(std::string cam_bin_path = "",
                                   std::vector<std::string> extra_args = {});
  ~SubprocessCameraBackend() override;

  SubprocessCameraBackend(const SubprocessCameraBackend&) = delete;
  SubprocessCameraBackend& operator=(const SubprocessCameraBackend&) = delete;
  SubprocessCameraBackend(SubprocessCameraBackend&&) = delete;
  SubprocessCameraBackend& operator=(SubprocessCameraBackend&&) = delete;

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

  pid_t child_pid() const;

 private:
  struct RawFrameSlot {
    uint64_t seq = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t pixfmt = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
  };

  std::string resolve_bin_path() const;
  bool send_configure(const CameraStreamSpec& spec,
                      std::string& out_err,
                      bool allow_before_ready = false);
  bool send_command(const std::string& cmd_name,
                    const std::string& json_body_without_braces,
                    int timeout_ms,
                    std::map<std::string, json_detail::JsonValue>& out_reply,
                    std::string& out_err,
                    bool allow_before_ready = false);
  void stdout_reader_loop(int stdout_fd, int wake_fd, pid_t pid);
  void frame_reader_loop(int frame_fd, int wake_fd);
  void handle_stdout_line(const std::string& line);
  void stop_locked();
  void close_stdin_fd();
  void cleanup_fds();

  std::string cam_bin_path_;
  std::vector<std::string> extra_args_;

  std::mutex lifecycle_mutex_;
  std::mutex cmd_mutex_;
  std::mutex write_mutex_;
  std::mutex reap_mutex_;
  mutable std::mutex state_mutex_;
  std::condition_variable reply_cv_;

  pid_t child_pid_ = -1;
  int stdin_write_fd_ = -1;
  int stdout_read_fd_ = -1;
  int frame_read_fd_ = -1;
  int wake_read_fd_ = -1;
  int wake_write_fd_ = -1;

  bool child_running_ = false;
  bool ready_ = false;
  bool recording_ = false;
  bool paused_ = false;
  bool audio_warning_ = false;
  bool stopping_ = false;
  bool process_dead_ = false;
  std::string last_error_;
  std::string backend_name_ = "picamera2-subprocess";

  bool ready_received_ = false;
  bool ready_ok_ = false;
  std::string ready_error_;

  uint64_t next_cmd_id_ = 1;
  uint64_t pending_cmd_id_ = 0;
  bool pending_reply_ready_ = false;
  std::map<std::string, json_detail::JsonValue> pending_reply_;

  CameraStreamSpec current_spec_;

  std::mutex frame_mutex_;
  RawFrameSlot latest_raw_;
  uint64_t frame_seq_counter_ = 0;

  std::thread stdout_thread_;
  std::thread frame_thread_;
};

}  // namespace cz
