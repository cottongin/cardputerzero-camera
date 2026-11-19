#pragma once

#include "camera_backend.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace cz {

namespace media_detail {

struct VideoProbeResult {
  bool ok = false;
  double duration_sec = 0.0;
  double fps = 24.0;
  int width = 0;
  int height = 0;
  bool has_audio = false;
  std::string error;
};

void ignore_sigpipe_once();
std::string resolve_executable(const std::string& name);
bool ends_with_ci(const std::string& s, const std::string& suffix);
bool spawn_stdout_process(const std::string& exe,
                          const std::vector<std::string>& args,
                          pid_t& out_pid,
                          int& out_stdout_fd);
bool reap_pid_timeout(pid_t pid, int timeout_ms, int* out_status = nullptr);
void kill_and_reap(pid_t& pid);
VideoProbeResult probe_video_file(const std::string& path);
bool compute_fit_dimensions(int src_w,
                            int src_h,
                            int max_w,
                            int max_h,
                            int& out_w,
                            int& out_h);
bool aspect_scale_rgb24(const uint8_t* src,
                        int src_w,
                        int src_h,
                        int max_w,
                        int max_h,
                        std::vector<uint8_t>& out_rgb,
                        int& out_w,
                        int& out_h);
bool read_exact_with_wake(int fd,
                          int wake_fd,
                          uint8_t* dst,
                          size_t n,
                          bool& out_woken,
                          bool& out_eof);

}  // namespace media_detail

struct VideoPlayerState {
  bool stopped = true;
  bool playing = false;
  bool paused = false;
  bool muted = false;
  bool eof = false;
  double position_sec = 0.0;
  double duration_sec = 0.0;
  int width = 0;
  int height = 0;
  std::string error;
  bool has_audio = false;
};

class VideoPlayer {
 public:
  VideoPlayer();
  ~VideoPlayer();

  VideoPlayer(const VideoPlayer&) = delete;
  VideoPlayer& operator=(const VideoPlayer&) = delete;

  bool open(const std::string& mp4_path, int max_w, int max_h);
  bool play(bool enable_audio = true);
  void pause();
  void toggle_mute();
  bool seek_delta(double delta_sec);
  bool poll_frame(PreviewFrame& out_frame);
  void set_pcm_sink(std::function<void(const int16_t*, size_t)> sink);
  void stop();
  VideoPlayerState state() const;

 private:
  void stop_workers_locked_ctrl();
  bool start_playback_locked_ctrl(double start_pos_sec, bool want_audio);
  bool start_audio_worker_locked_ctrl(double start_pos_sec);
  void stop_audio_worker_locked_ctrl();
  void video_worker_loop(double start_pos_sec,
                         int frame_w,
                         int frame_h,
                         double fps,
                         int stdout_fd,
                         int wake_fd,
                         pid_t child_pid);
  void audio_worker_pcm_loop(double start_pos_sec,
                             int stdout_fd,
                             int wake_fd,
                             pid_t child_pid,
                             std::function<void(const int16_t*, size_t)> sink);
  void audio_worker_aplay_loop(int wake_fd, pid_t ffmpeg_pid, pid_t aplay_pid);

  mutable std::mutex control_mutex_;
  mutable std::mutex state_mutex_;
  mutable std::mutex frame_mutex_;
  std::mutex proc_mutex_;
  std::condition_variable pace_cv_;

  std::string mp4_path_;
  int max_w_ = 226;
  int max_h_ = 128;
  double fps_ = 24.0;
  bool opened_ = false;
  VideoPlayerState state_;

  std::function<void(const int16_t*, size_t)> pcm_sink_;

  PreviewFrame latest_frame_;
  uint64_t seq_counter_ = 0;
  uint64_t last_polled_seq_ = 0;

  bool workers_stop_ = false;
  bool audio_stop_ = false;
  int video_wake_r_ = -1;
  int video_wake_w_ = -1;
  int audio_wake_r_ = -1;
  int audio_wake_w_ = -1;
  pid_t video_pid_ = -1;
  pid_t audio_ffmpeg_pid_ = -1;
  pid_t aplay_pid_ = -1;
  std::thread video_thread_;
  std::thread audio_thread_;
};

}  // namespace cz
