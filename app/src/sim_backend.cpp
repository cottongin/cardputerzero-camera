#include "sim_backend.hpp"

#include "jpeg_pipeline.hpp"
#include "subprocess_backend.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cz {

namespace {

std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

bool is_16_9_spec(const CameraStreamSpec& spec) {
  if (spec.mode == "video") {
    return true;
  }
  if (spec.main_height > 0) {
    const double aspect =
        static_cast<double>(spec.main_width) /
        static_cast<double>(spec.main_height);
    if (std::abs(aspect - (16.0 / 9.0)) < 0.05) {
      return true;
    }
  }
  return false;
}

uint8_t clamp_u8(float v) {
  if (v <= 0.0f) return 0;
  if (v >= 255.0f) return 255;
  return static_cast<uint8_t>(std::lround(v));
}

void render_procedural_scene_rgb24(int width,
                                   int height,
                                   uint64_t tick,
                                   float zoom,
                                   float ev,
                                   const std::string& transform,
                                   std::vector<uint8_t>& out_rgb24) {
  const int w = std::max(2, width);
  const int h = std::max(2, height);
  out_rgb24.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 3u);

  const float z = std::clamp(zoom, 1.0f, 4.0f);
  const float inv_z = 1.0f / z;
  const float ev_clamped = std::clamp(ev, -2.0f, 2.0f);
  const float ev_gain = std::pow(2.0f, ev_clamped * 0.45f);
  const float ev_bias = ev_clamped * 16.0f;

  const std::string tf = to_lower_ascii(transform);
  const bool flip_x = (tf == "180" || tf == "hflip");
  const bool flip_y = (tf == "180" || tf == "vflip");

  // Animated sun position
  const float phase = static_cast<float>(tick % 240u) / 240.0f;
  const float sun_u = 0.22f + 0.56f * phase;
  const float sun_v = 0.28f - 0.08f * std::sin(phase * 3.14159265f);
  const float sun_r2 = 0.0045f;

  // Precompute column-wise mountain ridges to keep 8MP rendering fast
  std::vector<float> col_u(static_cast<size_t>(w));
  std::vector<float> far_ridge(static_cast<size_t>(w));
  std::vector<float> near_ridge(static_cast<size_t>(w));
  for (int x = 0; x < w; ++x) {
    const float raw_u =
        (static_cast<float>(x) + 0.5f) / static_cast<float>(w);
    const float u_t = flip_x ? (1.0f - raw_u) : raw_u;
    const float u = 0.5f + (u_t - 0.5f) * inv_z;
    col_u[static_cast<size_t>(x)] = u;
    far_ridge[static_cast<size_t>(x)] =
        0.54f - 0.09f * std::sin(u * 9.0f + 0.4f) -
        0.04f * std::cos(u * 21.0f);
    near_ridge[static_cast<size_t>(x)] =
        0.66f - 0.07f * std::sin(u * 13.0f + 2.1f) -
        0.03f * std::sin(u * 29.0f);
  }

  for (int y = 0; y < h; ++y) {
    const float raw_v =
        (static_cast<float>(y) + 0.5f) / static_cast<float>(h);
    const float v_t = flip_y ? (1.0f - raw_v) : raw_v;
    const float v = 0.5f + (v_t - 0.5f) * inv_z;
    const float dv_sun = v - sun_v;
    uint8_t* row = out_rgb24.data() + static_cast<size_t>(y) * w * 3u;

    // Sky vertical gradient terms
    const float sky_t = std::clamp(v / 0.72f, 0.0f, 1.0f);
    const float sky_r = 28.0f + 168.0f * sky_t;
    const float sky_g = 54.0f + 112.0f * sky_t;
    const float sky_b = 112.0f + 46.0f * sky_t;

    const float wave =
        10.0f * std::sin(v * 80.0f - static_cast<float>(tick) * 0.25f);

    for (int x = 0; x < w; ++x) {
      const size_t sx = static_cast<size_t>(x);
      const float u = col_u[sx];
      float r = 0.0f;
      float g = 0.0f;
      float b = 0.0f;

      if (v >= 0.72f) {
        // Foreground lake + shimmer reflection
        const float depth = (v - 0.72f) / 0.28f;
        r = 22.0f + 18.0f * (1.0f - depth);
        g = 48.0f + 26.0f * (1.0f - depth);
        b = 76.0f + 28.0f * (1.0f - depth);
        const float sun_glint = std::exp(-((u - sun_u) * (u - sun_u)) * 90.0f);
        if (sun_glint > 0.08f) {
          const float shimmer = std::max(0.0f, wave) * sun_glint;
          r += 8.0f * shimmer;
          g += 5.5f * shimmer;
          b += 2.5f * shimmer;
        }
      } else if (v >= near_ridge[sx]) {
        // Near dark pine ridge
        r = 26.0f;
        g = 46.0f;
        b = 38.0f;
      } else if (v >= far_ridge[sx]) {
        // Far atmospheric mountain ridge
        r = 68.0f;
        g = 78.0f;
        b = 104.0f;
      } else {
        // Sky + sun disc
        r = sky_r;
        g = sky_g;
        b = sky_b;
        const float du = (u - sun_u) * 1.33f;  // circular in 4:3 aspect
        const float d2 = du * du + dv_sun * dv_sun;
        if (d2 < sun_r2) {
          r = 250.0f;
          g = 228.0f;
          b = 152.0f;
        } else if (d2 < sun_r2 * 5.0f) {
          const float halo = 1.0f - (d2 - sun_r2) / (sun_r2 * 4.0f);
          r += 55.0f * halo;
          g += 40.0f * halo;
          b += 15.0f * halo;
        }
      }

      row[x * 3 + 0] = clamp_u8(r * ev_gain + ev_bias);
      row[x * 3 + 1] = clamp_u8(g * ev_gain + ev_bias);
      row[x * 3 + 2] = clamp_u8(b * ev_gain + ev_bias);
    }
  }
}

bool write_minimal_mp4_stub(const std::string& path, std::string& out_err) {
  static constexpr uint8_t kStub[] = {
      0x00, 0x00, 0x00, 0x18, 'f',  't',  'y',  'p',  'm',  'p',  '4',  '2',
      0x00, 0x00, 0x00, 0x00, 'm',  'p',  '4',  '2',  'i',  's',  'o',  'm',
      0x00, 0x00, 0x00, 0x08, 'f',  'r',  'e',  'e',
      0x00, 0x00, 0x00, 0x10, 'm',  'd',  'a',  't',  0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
  };
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    out_err = "failed to write MP4 stub '" + path + "': " +
              std::strerror(errno);
    return false;
  }
  const size_t written = std::fwrite(kStub, 1, sizeof(kStub), fp);
  std::fflush(fp);
  std::fclose(fp);
  if (written != sizeof(kStub)) {
    out_err = "short write on MP4 stub";
    return false;
  }
  return true;
}

bool try_write_ffmpeg_mp4(const VideoSpec& spec, double duration_sec) {
  const double clamped_dur = std::clamp(duration_sec, 0.2, 3.0);
  const int w = std::max(64, std::min(640, spec.width & ~1));
  const int h = std::max(64, std::min(360, spec.height & ~1));
  const int fps = std::clamp(spec.fps, 10, 30);

  char dur_buf[32];
  std::snprintf(dur_buf, sizeof(dur_buf), "%.2f", clamped_dur);
  const std::string vsrc = "testsrc=duration=" + std::string(dur_buf) +
                           ":size=" + std::to_string(w) + "x" +
                           std::to_string(h) + ":rate=" + std::to_string(fps);
  const std::string asrc =
      "anullsrc=channel_layout=mono:sample_rate=44100";

  std::vector<std::string> args = {
      "ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
      vsrc};
  if (spec.audio_enabled) {
    args.push_back("-f");
    args.push_back("lavfi");
    args.push_back("-i");
    args.push_back(asrc);
    args.push_back("-shortest");
  }
  args.push_back("-c:v");
  args.push_back("libx264");
  args.push_back("-preset");
  args.push_back("ultrafast");
  args.push_back("-pix_fmt");
  args.push_back("yuv420p");
  if (spec.audio_enabled) {
    args.push_back("-c:a");
    args.push_back("aac");
    args.push_back("-b:a");
    args.push_back("64k");
  }
  args.push_back("-f");
  args.push_back("mp4");
  args.push_back("-movflags");
  args.push_back("+faststart");
  args.push_back(spec.tmp_path);

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args.size() + 1);
  for (auto& a : args) {
    argv_ptrs.push_back(const_cast<char*>(a.c_str()));
  }
  argv_ptrs.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > 2) ::close(devnull);
    }
    ::execvp("ffmpeg", argv_ptrs.data());
    _exit(127);
  }

  // Wait up to 4 seconds for short synthetic MP4 encode.
  const auto dl =
      std::chrono::steady_clock::now() + std::chrono::seconds(4);
  int st = 0;
  pid_t r = 0;
  while ((r = ::waitpid(pid, &st, WNOHANG)) == 0 &&
         std::chrono::steady_clock::now() < dl) {
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  if (r == 0) {
    ::kill(pid, SIGKILL);
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    return false;
  }
  if (r != pid || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    return false;
  }
  struct stat fst {};
  return ::stat(spec.tmp_path.c_str(), &fst) == 0 && fst.st_size > 0;
}

}  // namespace

SimCameraBackend::~SimCameraBackend() { stop(); }

void SimCameraBackend::publish_frame_locked_state(uint64_t tick,
                                                  const CameraStreamSpec& spec,
                                                  bool is_16_9) {
  const int pw = std::max(2, spec.preview_width & ~1);
  const int ph = std::max(2, spec.preview_height & ~1);
  std::vector<uint8_t> rgb;
  render_procedural_scene_rgb24(pw, ph, tick, spec.zoom, spec.ev,
                                spec.camera_transform, rgb);

  std::lock_guard<std::mutex> fr_lock(frame_mutex_);
  latest_frame_.seq = ++seq_counter_;
  latest_frame_.width = pw;
  latest_frame_.height = ph;
  latest_frame_.is_16_9 = is_16_9;
  latest_frame_.rgb24 = std::move(rgb);
}

bool SimCameraBackend::start(const CameraStreamSpec& initial_spec,
                             std::string& out_err) {
  stop();
  out_err.clear();

  CameraStreamSpec norm = initial_spec;
  if (norm.mode.empty()) norm.mode = "photo";
  if (norm.camera_transform.empty()) norm.camera_transform = "auto";
  norm.zoom = std::clamp(norm.zoom, 1.0f, 4.0f);
  norm.ev = std::clamp(norm.ev, -2.0f, 2.0f);
  norm.fps = std::clamp(norm.fps, 1, 60);

  uint64_t initial_tick = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    spec_ = norm;
    running_ = true;
    ready_ = true;
    paused_ = false;
    recording_ = false;
    stop_thread_ = false;
    last_error_.clear();
    initial_tick = tick_++;
  }

  publish_frame_locked_state(initial_tick, norm, is_16_9_spec(norm));
  render_thread_ = std::thread(&SimCameraBackend::render_loop, this);
  return true;
}

void SimCameraBackend::stop() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!running_ && !render_thread_.joinable()) {
      ready_ = false;
      recording_ = false;
      paused_ = false;
      return;
    }
    stop_thread_ = true;
    running_ = false;
    ready_ = false;
    recording_ = false;
    paused_ = false;
    state_cv_.notify_all();
  }
  if (render_thread_.joinable()) {
    render_thread_.join();
  }
}

void SimCameraBackend::render_loop() {
  while (true) {
    CameraStreamSpec snap_spec;
    uint64_t snap_tick = 0;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (stop_thread_) {
        return;
      }
      if (paused_ || !ready_) {
        state_cv_.wait(lock,
                       [&]() { return stop_thread_ || (!paused_ && ready_); });
        if (stop_thread_) {
          return;
        }
      }
      snap_spec = spec_;
      snap_tick = tick_++;
    }

    publish_frame_locked_state(snap_tick, snap_spec, is_16_9_spec(snap_spec));

    const int fps = std::clamp(snap_spec.fps, 1, 60);
    const auto period = std::chrono::milliseconds(1000 / fps);
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_cv_.wait_for(lock, period, [&]() { return stop_thread_; });
    if (stop_thread_) {
      return;
    }
  }
}

bool SimCameraBackend::reconfigure(const CameraStreamSpec& spec,
                                   std::string& out_err) {
  out_err.clear();
  CameraStreamSpec norm = spec;
  if (norm.mode.empty()) norm.mode = "photo";
  if (norm.camera_transform.empty()) norm.camera_transform = "auto";
  norm.zoom = std::clamp(norm.zoom, 1.0f, 4.0f);
  norm.ev = std::clamp(norm.ev, -2.0f, 2.0f);
  norm.fps = std::clamp(norm.fps, 1, 60);

  uint64_t cur_tick = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!ready_) {
      out_err = "sim camera backend not started";
      return false;
    }
    if (recording_) {
      out_err = "camera_recording";
      return false;
    }
    spec_ = norm;
    paused_ = false;
    cur_tick = tick_++;
    state_cv_.notify_all();
  }
  publish_frame_locked_state(cur_tick, norm, is_16_9_spec(norm));
  return true;
}

bool SimCameraBackend::set_controls(float zoom,
                                    float ev,
                                    std::string& out_err) {
  out_err.clear();
  CameraStreamSpec snap;
  uint64_t cur_tick = 0;
  bool should_publish = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!ready_) {
      out_err = "sim camera backend not started";
      return false;
    }
    spec_.zoom = std::clamp(zoom, 1.0f, 4.0f);
    spec_.ev = std::clamp(ev, -2.0f, 2.0f);
    snap = spec_;
    cur_tick = tick_++;
    should_publish = !paused_;
  }
  if (should_publish) {
    publish_frame_locked_state(cur_tick, snap, is_16_9_spec(snap));
  }
  return true;
}

bool SimCameraBackend::pause(std::string& out_err) {
  out_err.clear();
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!ready_) {
    out_err = "sim camera backend not started";
    return false;
  }
  recording_ = false;
  paused_ = true;
  state_cv_.notify_all();
  return true;
}

bool SimCameraBackend::resume(std::string& out_err) {
  out_err.clear();
  CameraStreamSpec snap;
  uint64_t cur_tick = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!ready_) {
      out_err = "sim camera backend not started";
      return false;
    }
    paused_ = false;
    snap = spec_;
    cur_tick = tick_++;
    state_cv_.notify_all();
  }
  publish_frame_locked_state(cur_tick, snap, is_16_9_spec(snap));
  return true;
}

bool SimCameraBackend::copy_latest_preview(uint64_t last_seen_seq,
                                           PreviewFrame& out_frame) {
  std::lock_guard<std::mutex> fr_lock(frame_mutex_);
  if (latest_frame_.seq == 0 || latest_frame_.seq <= last_seen_seq) {
    return false;
  }
  out_frame = latest_frame_;
  return true;
}

StillCaptureResult SimCameraBackend::capture_still(
    const std::string& tmp_jpeg_path,
    int width,
    int height) {
  StillCaptureResult res;
  if (tmp_jpeg_path.empty() || width <= 0 || height <= 0) {
    res.error = "invalid capture_still arguments";
    return res;
  }

  CameraStreamSpec snap;
  uint64_t cur_tick = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!ready_ || paused_) {
      res.error = "camera_not_started";
      return res;
    }
    if (recording_) {
      res.error = "camera_busy";
      return res;
    }
    snap = spec_;
    cur_tick = tick_;
  }

  const int req_w = std::max(2, width & ~1);
  const int req_h = std::max(2, height & ~1);
  std::vector<uint8_t> rgb;
  render_procedural_scene_rgb24(req_w, req_h, cur_tick, snap.zoom, snap.ev,
                                snap.camera_transform, rgb);

  if (!encode_jpeg_rgb24(tmp_jpeg_path, rgb.data(), req_w, req_h, 90,
                         res.error)) {
    return res;
  }

  struct stat st {};
  if (::stat(tmp_jpeg_path.c_str(), &st) != 0) {
    res.error = "failed to stat captured JPEG";
    return res;
  }

  res.ok = true;
  res.width = req_w;
  res.height = req_h;
  res.bytes = static_cast<uint64_t>(st.st_size);
  return res;
}

bool SimCameraBackend::start_video(const VideoSpec& spec,
                                   std::string& out_err) {
  out_err.clear();
  if (spec.tmp_path.empty() || spec.width <= 0 || spec.height <= 0 ||
      spec.fps <= 0) {
    out_err = "invalid start_video specification";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!ready_ || paused_) {
      out_err = "camera_not_started";
      return false;
    }
    if (recording_) {
      out_err = "already_recording";
      return false;
    }
    if (!write_minimal_mp4_stub(spec.tmp_path, out_err)) {
      return false;
    }
    active_video_ = spec;
    video_start_time_ = std::chrono::steady_clock::now();
    recording_ = true;
    spec_.mode = "video";
  }
  return true;
}

VideoStopResult SimCameraBackend::stop_video() {
  VideoStopResult res;
  VideoSpec spec;
  std::chrono::steady_clock::time_point t0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!recording_) {
      res.error = "not_recording";
      return res;
    }
    spec = active_video_;
    t0 = video_start_time_;
    recording_ = false;
    active_video_ = VideoSpec{};
  }

  const auto elapsed = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  const double duration_sec = std::max(0.001, elapsed);

  if (!try_write_ffmpeg_mp4(spec, duration_sec)) {
    if (!write_minimal_mp4_stub(spec.tmp_path, res.error)) {
      return res;
    }
  }

  struct stat st {};
  if (::stat(spec.tmp_path.c_str(), &st) != 0 || st.st_size <= 0) {
    res.error = "video staging file missing or empty";
    return res;
  }

  res.ok = true;
  res.duration_sec = duration_sec;
  res.bytes = static_cast<uint64_t>(st.st_size);
  res.audio_recorded = spec.audio_enabled;
  return res;
}

BackendHealth SimCameraBackend::status() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  BackendHealth h;
  h.ready = ready_;
  h.recording = recording_;
  h.paused = paused_;
  h.audio_warning = false;
  h.last_error = last_error_;
  h.backend_name = "sim";
  return h;
}

std::unique_ptr<ICameraBackend> create_camera_backend(bool force_mock) {
  if (force_mock) {
    return std::make_unique<SimCameraBackend>();
  }

  const char* env_backend = std::getenv("CZCAMERA_BACKEND");
  if (env_backend && *env_backend) {
    const std::string val = to_lower_ascii(env_backend);
    if (val == "sim" || val == "mock") {
      return std::make_unique<SimCameraBackend>();
    }
    if (val == "subprocess") {
      return std::make_unique<SubprocessCameraBackend>();
    }
  }

  const char* env_bin = std::getenv("CZCAMERA_CAM_BIN");
  if (env_bin && *env_bin) {
    return std::make_unique<SubprocessCameraBackend>();
  }

#ifdef CZ_TARGET_DEVICE
  return std::make_unique<SubprocessCameraBackend>();
#else
  return std::make_unique<SimCameraBackend>();
#endif
}

}  // namespace cz
