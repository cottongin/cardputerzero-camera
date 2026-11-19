#include "video_player.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <locale>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cz {

namespace media_detail {

namespace {

bool is_executable_regular_file(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return false;
  }
  return ::access(path.c_str(), X_OK) == 0;
}

void set_cloexec(int fd) {
  if (fd < 0) return;
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
}

void set_nonblocking(int fd) {
  if (fd < 0) return;
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags >= 0) {
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

void close_fd_safe(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

bool create_wake_pipe(int& out_r, int& out_w) {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    return false;
  }
  set_cloexec(fds[0]);
  set_cloexec(fds[1]);
  set_nonblocking(fds[0]);
  set_nonblocking(fds[1]);
  out_r = fds[0];
  out_w = fds[1];
  return true;
}

void signal_wake_pipe(int fd) {
  if (fd >= 0) {
    const uint8_t byte = 1;
    const ssize_t ignored = ::write(fd, &byte, 1);
    (void)ignored;
  }
}

std::string format_seconds_c_locale(double sec) {
  std::ostringstream oss;
  oss.imbue(std::locale::classic());
  oss.setf(std::ios::fixed);
  oss.precision(3);
  oss << std::max(0.0, sec);
  return oss.str();
}

double parse_rate_fraction(const std::string& s) {
  if (s.empty() || s == "N/A") {
    return 0.0;
  }
  const size_t slash = s.find('/');
  if (slash == std::string::npos) {
    const double val = std::strtod(s.c_str(), nullptr);
    return std::isfinite(val) && val > 0.0 ? val : 0.0;
  }
  const double num = std::strtod(s.substr(0, slash).c_str(), nullptr);
  const double den = std::strtod(s.substr(slash + 1).c_str(), nullptr);
  if (!std::isfinite(num) || !std::isfinite(den) || den <= 0.0 || num <= 0.0) {
    return 0.0;
  }
  return num / den;
}

double parse_double_safe(const std::string& s) {
  if (s.empty() || s == "N/A") {
    return 0.0;
  }
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || !std::isfinite(v) || v < 0.0) {
    return 0.0;
  }
  return v;
}

std::string trim_cr_ws(std::string s) {
  while (!s.empty() &&
         (s.back() == '\r' || s.back() == '\n' ||
          std::isspace(static_cast<unsigned char>(s.back())))) {
    s.pop_back();
  }
  return s;
}

}  // namespace

bool ends_with_ci(const std::string& s, const std::string& suffix) {
  if (s.size() < suffix.size()) {
    return false;
  }
  const size_t off = s.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
        std::tolower(static_cast<unsigned char>(suffix[i]))) {
      return false;
    }
  }
  return true;
}

bool spawn_stdout_process(const std::string& exe,
                          const std::vector<std::string>& args,
                          pid_t& out_pid,
                          int& out_stdout_fd) {
  out_pid = -1;
  out_stdout_fd = -1;

  int out_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0) {
    return false;
  }

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args.size() + 1);
  for (const auto& a : args) {
    argv_ptrs.push_back(const_cast<char*>(a.c_str()));
  }
  argv_ptrs.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return false;
  }
  if (pid == 0) {
    ::close(out_pipe[0]);
    if (::dup2(out_pipe[1], STDOUT_FILENO) < 0) {
      _exit(127);
    }
    if (out_pipe[1] > 2) {
      ::close(out_pipe[1]);
    }
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > 2) {
        ::close(devnull);
      }
    }
    ::execvp(exe.c_str(), argv_ptrs.data());
    _exit(127);
  }

  ::close(out_pipe[1]);
  set_cloexec(out_pipe[0]);
  set_nonblocking(out_pipe[0]);
  out_pid = pid;
  out_stdout_fd = out_pipe[0];
  return true;
}

void ignore_sigpipe_once() {
  static std::once_flag flag;
  std::call_once(flag, []() { std::signal(SIGPIPE, SIG_IGN); });
}

std::string resolve_executable(const std::string& name) {
  if (name.empty()) {
    return "";
  }
  if (name.find('/') != std::string::npos) {
    return is_executable_regular_file(name) ? name : "";
  }

  const char* path_env = std::getenv("PATH");
  if (path_env && *path_env) {
    std::string path_str(path_env);
    size_t start = 0;
    while (start <= path_str.size()) {
      const size_t colon = path_str.find(':', start);
      const std::string dir = (colon == std::string::npos)
                                  ? path_str.substr(start)
                                  : path_str.substr(start, colon - start);
      if (!dir.empty()) {
        const std::string cand =
            (dir.back() == '/') ? (dir + name) : (dir + "/" + name);
        if (is_executable_regular_file(cand)) {
          return cand;
        }
      }
      if (colon == std::string::npos) {
        break;
      }
      start = colon + 1;
    }
  }

  static constexpr const char* kFallbackDirs[] = {
      "/usr/local/bin",
      "/opt/homebrew/bin",
      "/usr/bin",
      "/bin",
  };
  for (const char* dir : kFallbackDirs) {
    const std::string cand = std::string(dir) + "/" + name;
    if (is_executable_regular_file(cand)) {
      return cand;
    }
  }
  return "";
}

bool reap_pid_timeout(pid_t pid, int timeout_ms, int* out_status) {
  if (pid <= 0) {
    return true;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    int st = 0;
    const pid_t wr = ::waitpid(pid, &st, WNOHANG);
    if (wr == pid) {
      if (out_status) *out_status = st;
      return true;
    }
    if (wr < 0) {
      if (errno == EINTR) {
        continue;
      }
      return errno == ECHILD;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void kill_and_reap(pid_t& pid) {
  if (pid <= 0) {
    pid = -1;
    return;
  }
  const pid_t target = pid;
  pid = -1;
  ::kill(target, SIGTERM);
  if (!reap_pid_timeout(target, 400, nullptr)) {
    ::kill(target, SIGKILL);
    int st = 0;
    while (::waitpid(target, &st, 0) < 0 && errno == EINTR) {
    }
  }
}

bool read_exact_with_wake(int fd,
                          int wake_fd,
                          uint8_t* dst,
                          size_t n,
                          bool& out_woken,
                          bool& out_eof) {
  out_woken = false;
  out_eof = false;
  size_t off = 0;
  while (off < n) {
    pollfd pfds[2] = {
        {fd, POLLIN, 0},
        {wake_fd, POLLIN, 0},
    };
    const int nfds = (wake_fd >= 0) ? 2 : 1;
    const int rc = ::poll(pfds, static_cast<nfds_t>(nfds), -1);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      out_eof = true;
      return false;
    }
    if (wake_fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      out_woken = true;
      return false;
    }
    if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      while (off < n) {
        const ssize_t r = ::read(fd, dst + off, n - off);
        if (r > 0) {
          off += static_cast<size_t>(r);
          continue;
        }
        if (r == 0) {
          out_eof = true;
          return false;
        }
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        out_eof = true;
        return false;
      }
    }
  }
  return true;
}

VideoProbeResult probe_video_file(const std::string& path) {
  VideoProbeResult res;
  if (path.empty()) {
    res.error = "empty video path";
    return res;
  }
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    res.error = "video file missing or empty";
    return res;
  }

  const std::string ffprobe = resolve_executable("ffprobe");
  if (ffprobe.empty()) {
    res.error = "ffprobe executable not found";
    return res;
  }

  ignore_sigpipe_once();

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0) {
    res.error = "pipe failed";
    return res;
  }
  if (::pipe(err_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    res.error = "pipe failed";
    return res;
  }

  std::vector<std::string> args = {
      ffprobe,
      "-v",
      "error",
      "-show_entries",
      "stream=codec_type,width,height,r_frame_rate,avg_frame_rate,duration",
      "-show_entries",
      "format=duration",
      "-of",
      "default",
      path,
  };
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args.size() + 1);
  for (auto& a : args) {
    argv_ptrs.push_back(const_cast<char*>(a.c_str()));
  }
  argv_ptrs.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    res.error = "fork failed";
    return res;
  }
  if (pid == 0) {
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    if (out_pipe[1] > 2) ::close(out_pipe[1]);
    if (err_pipe[1] > 2) ::close(err_pipe[1]);
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      if (devnull > 2) ::close(devnull);
    }
    ::execvp(ffprobe.c_str(), argv_ptrs.data());
    _exit(127);
  }

  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  set_nonblocking(out_pipe[0]);
  set_nonblocking(err_pipe[0]);

  std::string stdout_text;
  std::string stderr_text;
  bool out_open = true;
  bool err_open = true;
  const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);

  while ((out_open || err_open) && std::chrono::steady_clock::now() < dl) {
    pollfd pfds[2]{};
    nfds_t nfds = 0;
    int out_idx = -1;
    int err_idx = -1;
    if (out_open) {
      out_idx = static_cast<int>(nfds);
      pfds[nfds++] = {out_pipe[0], POLLIN, 0};
    }
    if (err_open) {
      err_idx = static_cast<int>(nfds);
      pfds[nfds++] = {err_pipe[0], POLLIN, 0};
    }
    const int rc = ::poll(pfds, nfds, 100);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    auto drain = [](int fd, std::string& dst, bool& open_flag) {
      char buf[1024];
      while (true) {
        const ssize_t r = ::read(fd, buf, sizeof(buf));
        if (r > 0) {
          if (dst.size() < 65536) {
            dst.append(buf, static_cast<size_t>(r));
          }
          continue;
        }
        if (r == 0) {
          open_flag = false;
          break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        open_flag = false;
        break;
      }
    };
    if (out_idx >= 0 && (pfds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
      drain(out_pipe[0], stdout_text, out_open);
    }
    if (err_idx >= 0 && (pfds[err_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
      drain(err_pipe[0], stderr_text, err_open);
    }
  }

  ::close(out_pipe[0]);
  ::close(err_pipe[0]);

  int status = 0;
  if (!reap_pid_timeout(pid, 500, &status)) {
    pid_t tmp_pid = pid;
    kill_and_reap(tmp_pid);
    res.error = "ffprobe timed out";
    return res;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    res.error = stderr_text.empty() ? "ffprobe failed" : trim_cr_ws(stderr_text);
    return res;
  }

  enum class Section { None, Stream, Format };
  Section sec = Section::None;
  std::string codec_type;
  int stream_w = 0;
  int stream_h = 0;
  double stream_avg_fps = 0.0;
  double stream_r_fps = 0.0;
  double stream_dur = 0.0;
  double format_dur = 0.0;
  double video_stream_dur = 0.0;

  auto flush_stream = [&]() {
    if (codec_type == "video" && res.width == 0 && stream_w > 0 && stream_h > 0) {
      res.width = stream_w;
      res.height = stream_h;
      const double f = stream_avg_fps > 0.0 ? stream_avg_fps : stream_r_fps;
      if (f > 0.0) {
        res.fps = std::clamp(f, 1.0, 120.0);
      }
      if (stream_dur > 0.0) {
        video_stream_dur = stream_dur;
      }
    } else if (codec_type == "audio") {
      res.has_audio = true;
    }
    codec_type.clear();
    stream_w = 0;
    stream_h = 0;
    stream_avg_fps = 0.0;
    stream_r_fps = 0.0;
    stream_dur = 0.0;
  };

  std::istringstream iss(stdout_text);
  std::string line;
  while (std::getline(iss, line)) {
    line = trim_cr_ws(line);
    if (line.empty()) continue;
    if (line == "[STREAM]") {
      sec = Section::Stream;
      codec_type.clear();
      stream_w = 0;
      stream_h = 0;
      stream_avg_fps = 0.0;
      stream_r_fps = 0.0;
      stream_dur = 0.0;
      continue;
    }
    if (line == "[/STREAM]") {
      flush_stream();
      sec = Section::None;
      continue;
    }
    if (line == "[FORMAT]") {
      sec = Section::Format;
      continue;
    }
    if (line == "[/FORMAT]") {
      sec = Section::None;
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string val = line.substr(eq + 1);
    if (sec == Section::Stream) {
      if (key == "codec_type") codec_type = val;
      else if (key == "width") stream_w = std::atoi(val.c_str());
      else if (key == "height") stream_h = std::atoi(val.c_str());
      else if (key == "avg_frame_rate") stream_avg_fps = parse_rate_fraction(val);
      else if (key == "r_frame_rate") stream_r_fps = parse_rate_fraction(val);
      else if (key == "duration") stream_dur = parse_double_safe(val);
    } else if (sec == Section::Format) {
      if (key == "duration") format_dur = parse_double_safe(val);
    }
  }

  res.duration_sec = format_dur > 0.0 ? format_dur : video_stream_dur;
  if (res.width <= 0 || res.height <= 0) {
    res.error = "no valid video stream found";
    return res;
  }

  res.ok = true;
  res.error.clear();
  return res;
}

bool compute_fit_dimensions(int src_w,
                            int src_h,
                            int max_w,
                            int max_h,
                            int& out_w,
                            int& out_h) {
  out_w = 0;
  out_h = 0;
  if (src_w <= 0 || src_h <= 0 || max_w <= 1 || max_h <= 1) {
    return false;
  }
  const double scale =
      std::min(1.0, std::min(static_cast<double>(max_w) / src_w,
                             static_cast<double>(max_h) / src_h));
  int w = static_cast<int>(std::floor(src_w * scale)) & ~1;
  int h = static_cast<int>(std::floor(src_h * scale)) & ~1;
  w = std::clamp(w, 2, max_w & ~1);
  h = std::clamp(h, 2, max_h & ~1);
  out_w = w;
  out_h = h;
  return true;
}

bool aspect_scale_rgb24(const uint8_t* src,
                        int src_w,
                        int src_h,
                        int max_w,
                        int max_h,
                        std::vector<uint8_t>& out_rgb,
                        int& out_w,
                        int& out_h) {
  out_rgb.clear();
  out_w = 0;
  out_h = 0;
  if (!src || src_w <= 0 || src_h <= 0 || max_w <= 0 || max_h <= 0) {
    return false;
  }

  if (src_w <= max_w && src_h <= max_h) {
    out_w = src_w;
    out_h = src_h;
    const size_t total = static_cast<size_t>(src_w) * src_h * 3u;
    out_rgb.assign(src, src + total);
    return true;
  }

  const double scale = std::min(static_cast<double>(max_w) / src_w,
                                static_cast<double>(max_h) / src_h);
  out_w = std::clamp(static_cast<int>(std::floor(src_w * scale)), 1, max_w);
  out_h = std::clamp(static_cast<int>(std::floor(src_h * scale)), 1, max_h);

  out_rgb.resize(static_cast<size_t>(out_w) * out_h * 3u);
  for (int y = 0; y < out_h; ++y) {
    const double sy =
        (static_cast<double>(y) + 0.5) * src_h / static_cast<double>(out_h) - 0.5;
    const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src_h - 1);
    const int y1 = std::min(y0 + 1, src_h - 1);
    const double fy = std::clamp(sy - y0, 0.0, 1.0);

    for (int x = 0; x < out_w; ++x) {
      const double sx =
          (static_cast<double>(x) + 0.5) * src_w / static_cast<double>(out_w) - 0.5;
      const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src_w - 1);
      const int x1 = std::min(x0 + 1, src_w - 1);
      const double fx = std::clamp(sx - x0, 0.0, 1.0);

      const uint8_t* p00 = src + (static_cast<size_t>(y0) * src_w + x0) * 3u;
      const uint8_t* p10 = src + (static_cast<size_t>(y0) * src_w + x1) * 3u;
      const uint8_t* p01 = src + (static_cast<size_t>(y1) * src_w + x0) * 3u;
      const uint8_t* p11 = src + (static_cast<size_t>(y1) * src_w + x1) * 3u;

      uint8_t* dst = out_rgb.data() + (static_cast<size_t>(y) * out_w + x) * 3u;
      for (int c = 0; c < 3; ++c) {
        const double top = p00[c] * (1.0 - fx) + p10[c] * fx;
        const double bot = p01[c] * (1.0 - fx) + p11[c] * fx;
        const double v = top * (1.0 - fy) + bot * fy;
        dst[c] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
      }
    }
  }
  return true;
}

}  // namespace media_detail

VideoPlayer::VideoPlayer() { media_detail::ignore_sigpipe_once(); }

VideoPlayer::~VideoPlayer() { stop(); }

void VideoPlayer::set_pcm_sink(
    std::function<void(const int16_t*, size_t)> sink) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  pcm_sink_ = std::move(sink);
}

bool VideoPlayer::open(const std::string& mp4_path, int max_w, int max_h) {
  std::lock_guard<std::mutex> ctrl(control_mutex_);
  stop_workers_locked_ctrl();

  const media_detail::VideoProbeResult probe =
      media_detail::probe_video_file(mp4_path);
  int target_w = 0;
  int target_h = 0;
  const bool dims_ok =
      probe.ok && media_detail::compute_fit_dimensions(probe.width, probe.height,
                                                       max_w, max_h, target_w,
                                                       target_h);

  {
    std::lock_guard<std::mutex> fr_lock(frame_mutex_);
    latest_frame_ = PreviewFrame{};
    last_polled_seq_ = seq_counter_;
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!probe.ok || !dims_ok) {
    opened_ = false;
    mp4_path_.clear();
    state_ = VideoPlayerState{};
    state_.error = probe.error.empty() ? "invalid video dimensions" : probe.error;
    return false;
  }

  mp4_path_ = mp4_path;
  max_w_ = max_w;
  max_h_ = max_h;
  fps_ = probe.fps;
  opened_ = true;

  state_ = VideoPlayerState{};
  state_.stopped = true;
  state_.playing = false;
  state_.paused = false;
  state_.muted = false;
  state_.eof = false;
  state_.position_sec = 0.0;
  state_.duration_sec = probe.duration_sec;
  state_.width = target_w;
  state_.height = target_h;
  state_.has_audio = probe.has_audio;
  state_.error.clear();
  return true;
}

bool VideoPlayer::play(bool enable_audio) {
  std::lock_guard<std::mutex> ctrl(control_mutex_);

  bool already_playing = false;
  double start_pos = 0.0;
  bool has_audio = false;
  bool was_muted = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!opened_ || mp4_path_.empty()) {
      state_.error = "no video opened";
      return false;
    }
    already_playing = state_.playing;
    if (state_.eof) {
      state_.position_sec = 0.0;
      state_.eof = false;
    }
    start_pos = state_.position_sec;
    has_audio = state_.has_audio;
    was_muted = state_.muted;
    state_.muted = !enable_audio;
  }

  const bool want_audio = enable_audio && has_audio;
  if (already_playing) {
    if (was_muted != !enable_audio) {
      if (want_audio) {
        start_audio_worker_locked_ctrl(start_pos);
      } else {
        stop_audio_worker_locked_ctrl();
      }
    }
    return true;
  }

  return start_playback_locked_ctrl(start_pos, want_audio);
}

void VideoPlayer::pause() {
  std::lock_guard<std::mutex> ctrl(control_mutex_);
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!opened_ || state_.stopped) {
      return;
    }
    if (state_.paused && !state_.playing) {
      return;
    }
  }
  stop_workers_locked_ctrl();
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!state_.eof) {
    state_.playing = false;
    state_.paused = true;
    state_.stopped = false;
  }
}

void VideoPlayer::toggle_mute() {
  std::lock_guard<std::mutex> ctrl(control_mutex_);
  bool is_playing = false;
  bool now_muted = false;
  bool has_audio = false;
  double pos = 0.0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.muted = !state_.muted;
    now_muted = state_.muted;
    is_playing = state_.playing;
    has_audio = state_.has_audio;
    pos = state_.position_sec;
  }
  if (is_playing && has_audio) {
    if (now_muted) {
      stop_audio_worker_locked_ctrl();
    } else {
      start_audio_worker_locked_ctrl(pos);
    }
  }
}

bool VideoPlayer::seek_delta(double delta_sec) {
  std::lock_guard<std::mutex> ctrl(control_mutex_);
  bool was_playing = false;
  bool want_audio = false;
  double new_pos = 0.0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!opened_ || mp4_path_.empty()) {
      return false;
    }
    was_playing = state_.playing;
    want_audio = state_.has_audio && !state_.muted;
    const double max_pos = state_.duration_sec > 0.1
                               ? (state_.duration_sec - 0.05)
                               : state_.duration_sec;
    new_pos = std::clamp(state_.position_sec + delta_sec, 0.0,
                         std::max(0.0, max_pos));
  }

  stop_workers_locked_ctrl();

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.position_sec = new_pos;
    state_.eof = false;
    if (!was_playing && state_.stopped) {
      state_.stopped = true;
      state_.paused = false;
    }
  }

  if (was_playing) {
    return start_playback_locked_ctrl(new_pos, want_audio);
  }
  return true;
}

bool VideoPlayer::poll_frame(PreviewFrame& out_frame) {
  std::lock_guard<std::mutex> fr_lock(frame_mutex_);
  if (latest_frame_.seq == 0 || latest_frame_.seq <= last_polled_seq_) {
    return false;
  }
  out_frame = latest_frame_;
  last_polled_seq_ = latest_frame_.seq;
  return true;
}

void VideoPlayer::stop() {
  std::lock_guard<std::mutex> ctrl(control_mutex_);
  stop_workers_locked_ctrl();
  std::lock_guard<std::mutex> lock(state_mutex_);
  state_.playing = false;
  state_.paused = false;
  state_.stopped = true;
  state_.eof = false;
  state_.position_sec = 0.0;
}

VideoPlayerState VideoPlayer::state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return state_;
}

bool VideoPlayer::start_playback_locked_ctrl(double start_pos_sec,
                                             bool want_audio) {
  stop_workers_locked_ctrl();

  const std::string ffmpeg = media_detail::resolve_executable("ffmpeg");
  if (ffmpeg.empty()) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.error = "ffmpeg executable not found";
    state_.playing = false;
    state_.stopped = true;
    return false;
  }

  std::string path;
  int w = 0;
  int h = 0;
  double fps = 24.0;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    path = mp4_path_;
    w = state_.width;
    h = state_.height;
    fps = fps_;
  }

  if (!media_detail::create_wake_pipe(video_wake_r_, video_wake_w_)) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.error = "failed to create wakeup pipe";
    return false;
  }

  std::vector<std::string> vargs = {
      ffmpeg,
      "-nostdin",
      "-loglevel",
      "error",
  };
  if (start_pos_sec > 0.001) {
    vargs.push_back("-ss");
    vargs.push_back(media_detail::format_seconds_c_locale(start_pos_sec));
  }
  vargs.push_back("-i");
  vargs.push_back(path);
  vargs.push_back("-vf");
  vargs.push_back("scale=" + std::to_string(w) + ":" + std::to_string(h));
  vargs.push_back("-f");
  vargs.push_back("rawvideo");
  vargs.push_back("-pix_fmt");
  vargs.push_back("rgb24");
  vargs.push_back("-");

  pid_t vpid = -1;
  int vfd = -1;
  if (!media_detail::spawn_stdout_process(ffmpeg, vargs, vpid, vfd)) {
    media_detail::close_fd_safe(video_wake_r_);
    media_detail::close_fd_safe(video_wake_w_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    state_.error = "failed to spawn ffmpeg video decoder";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    workers_stop_ = false;
    state_.stopped = false;
    state_.paused = false;
    state_.playing = true;
    state_.eof = false;
    state_.position_sec = start_pos_sec;
    state_.error.clear();
  }
  {
    std::lock_guard<std::mutex> plock(proc_mutex_);
    video_pid_ = vpid;
  }

  video_thread_ = std::thread(&VideoPlayer::video_worker_loop, this,
                              start_pos_sec, w, h, fps, vfd, video_wake_r_,
                              vpid);

  if (want_audio) {
    start_audio_worker_locked_ctrl(start_pos_sec);
  }
  return true;
}

bool VideoPlayer::start_audio_worker_locked_ctrl(double start_pos_sec) {
  stop_audio_worker_locked_ctrl();

  const std::string ffmpeg = media_detail::resolve_executable("ffmpeg");
  if (ffmpeg.empty()) {
    return false;
  }

  std::string path;
  std::function<void(const int16_t*, size_t)> sink;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!state_.has_audio || state_.muted || !state_.playing) {
      return false;
    }
    path = mp4_path_;
    sink = pcm_sink_;
  }

  if (!media_detail::create_wake_pipe(audio_wake_r_, audio_wake_w_)) {
    return false;
  }

  std::vector<std::string> aargs = {
      ffmpeg,
      "-nostdin",
      "-loglevel",
      "error",
  };
  if (start_pos_sec > 0.001) {
    aargs.push_back("-ss");
    aargs.push_back(media_detail::format_seconds_c_locale(start_pos_sec));
  }
  aargs.push_back("-i");
  aargs.push_back(path);
  aargs.push_back("-vn");
  aargs.push_back("-f");
  aargs.push_back("s16le");
  aargs.push_back("-ac");
  aargs.push_back("1");
  aargs.push_back("-ar");
  aargs.push_back("44100");
  aargs.push_back("-");

  if (sink) {
    pid_t apid = -1;
    int afd = -1;
    if (!media_detail::spawn_stdout_process(ffmpeg, aargs, apid, afd)) {
      media_detail::close_fd_safe(audio_wake_r_);
      media_detail::close_fd_safe(audio_wake_w_);
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      audio_stop_ = false;
    }
    {
      std::lock_guard<std::mutex> plock(proc_mutex_);
      audio_ffmpeg_pid_ = apid;
    }
    audio_thread_ = std::thread(&VideoPlayer::audio_worker_pcm_loop, this,
                                start_pos_sec, afd, audio_wake_r_, apid,
                                std::move(sink));
    return true;
  }

  const std::string aplay = media_detail::resolve_executable("aplay");
  if (aplay.empty()) {
    media_detail::close_fd_safe(audio_wake_r_);
    media_detail::close_fd_safe(audio_wake_w_);
    return false;
  }

  int pcm_pipe[2] = {-1, -1};
  if (::pipe(pcm_pipe) != 0) {
    media_detail::close_fd_safe(audio_wake_r_);
    media_detail::close_fd_safe(audio_wake_w_);
    return false;
  }

  std::vector<char*> ff_argv;
  ff_argv.reserve(aargs.size() + 1);
  for (auto& s : aargs) ff_argv.push_back(const_cast<char*>(s.c_str()));
  ff_argv.push_back(nullptr);

  std::vector<std::string> p_args = {
      aplay, "-f", "S16_LE", "-r", "44100", "-c", "1", "-q",
  };
  std::vector<char*> ap_argv;
  ap_argv.reserve(p_args.size() + 1);
  for (auto& s : p_args) ap_argv.push_back(const_cast<char*>(s.c_str()));
  ap_argv.push_back(nullptr);

  const pid_t ff_pid = ::fork();
  if (ff_pid < 0) {
    ::close(pcm_pipe[0]);
    ::close(pcm_pipe[1]);
    media_detail::close_fd_safe(audio_wake_r_);
    media_detail::close_fd_safe(audio_wake_w_);
    return false;
  }
  if (ff_pid == 0) {
    ::close(pcm_pipe[0]);
    ::dup2(pcm_pipe[1], STDOUT_FILENO);
    if (pcm_pipe[1] > 2) ::close(pcm_pipe[1]);
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > 2) ::close(devnull);
    }
    ::execvp(ffmpeg.c_str(), ff_argv.data());
    _exit(127);
  }

  const pid_t ap_pid = ::fork();
  if (ap_pid < 0) {
    ::close(pcm_pipe[0]);
    ::close(pcm_pipe[1]);
    pid_t tmp = ff_pid;
    media_detail::kill_and_reap(tmp);
    media_detail::close_fd_safe(audio_wake_r_);
    media_detail::close_fd_safe(audio_wake_w_);
    return false;
  }
  if (ap_pid == 0) {
    ::close(pcm_pipe[1]);
    ::dup2(pcm_pipe[0], STDIN_FILENO);
    if (pcm_pipe[0] > 2) ::close(pcm_pipe[0]);
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > 2) ::close(devnull);
    }
    ::execvp(aplay.c_str(), ap_argv.data());
    _exit(127);
  }

  ::close(pcm_pipe[0]);
  ::close(pcm_pipe[1]);

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    audio_stop_ = false;
  }
  {
    std::lock_guard<std::mutex> plock(proc_mutex_);
    audio_ffmpeg_pid_ = ff_pid;
    aplay_pid_ = ap_pid;
  }
  audio_thread_ = std::thread(&VideoPlayer::audio_worker_aplay_loop, this,
                              audio_wake_r_, ff_pid, ap_pid);
  return true;
}

void VideoPlayer::stop_audio_worker_locked_ctrl() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    audio_stop_ = true;
  }
  pace_cv_.notify_all();
  media_detail::signal_wake_pipe(audio_wake_w_);

  {
    std::lock_guard<std::mutex> plock(proc_mutex_);
    media_detail::kill_and_reap(aplay_pid_);
    media_detail::kill_and_reap(audio_ffmpeg_pid_);
  }

  if (audio_thread_.joinable()) {
    audio_thread_.join();
  }

  media_detail::close_fd_safe(audio_wake_r_);
  media_detail::close_fd_safe(audio_wake_w_);
}

void VideoPlayer::stop_workers_locked_ctrl() {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    workers_stop_ = true;
    audio_stop_ = true;
  }
  pace_cv_.notify_all();
  media_detail::signal_wake_pipe(video_wake_w_);
  media_detail::signal_wake_pipe(audio_wake_w_);

  {
    std::lock_guard<std::mutex> plock(proc_mutex_);
    media_detail::kill_and_reap(video_pid_);
    media_detail::kill_and_reap(aplay_pid_);
    media_detail::kill_and_reap(audio_ffmpeg_pid_);
  }

  if (video_thread_.joinable()) {
    video_thread_.join();
  }
  if (audio_thread_.joinable()) {
    audio_thread_.join();
  }

  media_detail::close_fd_safe(video_wake_r_);
  media_detail::close_fd_safe(video_wake_w_);
  media_detail::close_fd_safe(audio_wake_r_);
  media_detail::close_fd_safe(audio_wake_w_);
}

void VideoPlayer::video_worker_loop(double start_pos_sec,
                                    int frame_w,
                                    int frame_h,
                                    double fps,
                                    int stdout_fd,
                                    int wake_fd,
                                    pid_t child_pid) {
  const size_t frame_bytes =
      static_cast<size_t>(frame_w) * static_cast<size_t>(frame_h) * 3u;
  const bool is_16_9 =
      std::abs(static_cast<double>(frame_w) / frame_h - (16.0 / 9.0)) < 0.05;
  const auto frame_period = std::chrono::microseconds(
      static_cast<int64_t>(std::llround(1000000.0 / std::max(1.0, fps))));
  const auto playback_t0 = std::chrono::steady_clock::now();

  std::vector<uint8_t> buf(frame_bytes);
  uint64_t frame_idx = 0;
  bool natural_eof = false;

  while (true) {
    bool woken = false;
    bool hit_eof = false;
    if (!media_detail::read_exact_with_wake(stdout_fd, wake_fd, buf.data(),
                                            frame_bytes, woken, hit_eof)) {
      if (hit_eof && !woken) {
        natural_eof = true;
      }
      break;
    }

    if (frame_idx > 0) {
      const auto target_time = playback_t0 + frame_period * frame_idx;
      std::unique_lock<std::mutex> lock(state_mutex_);
      pace_cv_.wait_until(lock, target_time, [&]() { return workers_stop_; });
      if (workers_stop_) {
        break;
      }
    }

    {
      std::lock_guard<std::mutex> fr_lock(frame_mutex_);
      latest_frame_.seq = ++seq_counter_;
      latest_frame_.width = frame_w;
      latest_frame_.height = frame_h;
      latest_frame_.is_16_9 = is_16_9;
      latest_frame_.rgb24 = buf;
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (workers_stop_) {
        break;
      }
      const double cur_pos = start_pos_sec + static_cast<double>(frame_idx) / fps;
      state_.position_sec = state_.duration_sec > 0.0
                                ? std::min(cur_pos, state_.duration_sec)
                                : cur_pos;
    }
    ++frame_idx;
  }

  ::close(stdout_fd);

  bool stopped_by_caller = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    stopped_by_caller = workers_stop_;
  }

  int status = 0;
  if (!stopped_by_caller && natural_eof) {
    {
      std::lock_guard<std::mutex> plock(proc_mutex_);
      if (video_pid_ == child_pid) {
        video_pid_ = -1;
      }
    }
    if (!media_detail::reap_pid_timeout(child_pid, 500, &status)) {
      pid_t tmp = child_pid;
      media_detail::kill_and_reap(tmp);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!workers_stop_) {
      state_.playing = false;
      state_.paused = false;
      state_.stopped = true;
      state_.eof = true;
      if (state_.duration_sec > 0.0) {
        state_.position_sec = state_.duration_sec;
      }
      if (WIFEXITED(status) && WEXITSTATUS(status) != 0 && frame_idx == 0) {
        state_.error = "ffmpeg video decoder exited with code " +
                       std::to_string(WEXITSTATUS(status));
      }
    }
  }
}

void VideoPlayer::audio_worker_pcm_loop(
    double /*start_pos_sec*/,
    int stdout_fd,
    int wake_fd,
    pid_t child_pid,
    std::function<void(const int16_t*, size_t)> sink) {
  static constexpr size_t kChunkSamples = 2048;
  std::vector<int16_t> samples(kChunkSamples);
  uint8_t* raw_bytes = reinterpret_cast<uint8_t*>(samples.data());
  size_t leftover = 0;
  const auto t0 = std::chrono::steady_clock::now();
  uint64_t total_samples = 0;

  while (true) {
    pollfd pfds[2] = {
        {stdout_fd, POLLIN, 0},
        {wake_fd, POLLIN, 0},
    };
    const int rc = ::poll(pfds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      break;
    }
    if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      const ssize_t r = ::read(
          stdout_fd, raw_bytes + leftover,
          kChunkSamples * sizeof(int16_t) - leftover);
      if (r <= 0) {
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
          continue;
        }
        break;
      }
      const size_t total_bytes = leftover + static_cast<size_t>(r);
      const size_t count = total_bytes / sizeof(int16_t);
      const size_t next_leftover = total_bytes % sizeof(int16_t);
      const uint8_t carry_byte =
          next_leftover > 0 ? raw_bytes[total_bytes - 1] : 0;
      if (count > 0 && sink) {
        sink(samples.data(), count);
        total_samples += count;

        // Keep audio queued ~150ms ahead of playback without flooding SDL queue
        const double audio_sec = static_cast<double>(total_samples) / 44100.0;
        if (audio_sec > 0.15) {
          const auto target =
              t0 + std::chrono::microseconds(static_cast<int64_t>(
                       std::llround((audio_sec - 0.15) * 1000000.0)));
          std::unique_lock<std::mutex> lock(state_mutex_);
          pace_cv_.wait_until(lock, target,
                              [&]() { return workers_stop_ || audio_stop_; });
          if (workers_stop_ || audio_stop_) {
            break;
          }
        }
      }
      leftover = next_leftover;
      if (leftover > 0) {
        raw_bytes[0] = carry_byte;
      }
    }
  }

  ::close(stdout_fd);
  bool stopped = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    stopped = workers_stop_ || audio_stop_;
  }
  if (!stopped) {
    std::lock_guard<std::mutex> plock(proc_mutex_);
    if (audio_ffmpeg_pid_ == child_pid) {
      audio_ffmpeg_pid_ = -1;
      media_detail::reap_pid_timeout(child_pid, 400, nullptr);
    }
  }
}

void VideoPlayer::audio_worker_aplay_loop(int wake_fd,
                                          pid_t ffmpeg_pid,
                                          pid_t aplay_pid) {
  while (true) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (workers_stop_ || audio_stop_) {
        break;
      }
    }
    int st_ap = 0;
    const pid_t r_ap = ::waitpid(aplay_pid, &st_ap, WNOHANG);
    if (r_ap == aplay_pid || (r_ap < 0 && errno == ECHILD)) {
      std::lock_guard<std::mutex> plock(proc_mutex_);
      if (aplay_pid_ == aplay_pid) {
        aplay_pid_ = -1;
      }
      if (audio_ffmpeg_pid_ == ffmpeg_pid) {
        media_detail::kill_and_reap(audio_ffmpeg_pid_);
      }
      break;
    }

    pollfd pfd{wake_fd, POLLIN, 0};
    const int rc = ::poll(&pfd, 1, 50);
    if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
      break;
    }
  }
}

}  // namespace cz
