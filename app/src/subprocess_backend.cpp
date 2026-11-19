#include "subprocess_backend.hpp"

#include "config.hpp"
#include "storage.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cz {

namespace {

constexpr int kChildFrameFd = 3;
constexpr size_t kMaxLogBytes = 5u * 1024u * 1024u;
constexpr int kReadyTimeoutMs = 5000;
constexpr int kCommandTimeoutMs = 5000;
constexpr int kStopVideoTimeoutMs = 10000;
constexpr int kShutdownTimeoutMs = 1000;

void ignore_sigpipe_once() {
  static std::once_flag flag;
  std::call_once(flag, []() { std::signal(SIGPIPE, SIG_IGN); });
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

bool write_all_fd(int fd, const char* data, size_t len, std::string& out_err) {
  size_t off = 0;
  while (off < len) {
    const ssize_t w = ::write(fd, data + off, len - off);
    if (w > 0) {
      off += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) {
      continue;
    }
    const int err_no = (w < 0) ? errno : EPIPE;
    out_err = std::string("write to camera helper failed: ") +
              std::strerror(err_no);
    return false;
  }
  return true;
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

std::string resolve_stderr_log_path() {
  const char* env_log = std::getenv("CZCAMERA_LOG");
  if (env_log && *env_log) {
    return expand_user_path(env_log);
  }
  return expand_user_path("~/.local/share/czcamera/czcamera.log");
}

int open_stderr_log_fd() {
  const std::string log_path = resolve_stderr_log_path();
  if (log_path.empty()) {
    return -1;
  }
  const size_t slash = log_path.find_last_of('/');
  if (slash != std::string::npos) {
    const std::string parent = slash == 0 ? "/" : log_path.substr(0, slash);
    mkdir_p(parent);
  }
  struct stat st {};
  if (::stat(log_path.c_str(), &st) == 0 &&
      static_cast<size_t>(st.st_size) >= kMaxLogBytes) {
    const std::string rot1 = log_path + ".1";
    const std::string rot2 = log_path + ".2";
    ::rename(rot1.c_str(), rot2.c_str());
    ::rename(log_path.c_str(), rot1.c_str());
  }
  return ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                0644);
}

bool move_fd_above_3(int& fd) {
  if (fd < 0 || fd >= 4) {
    return true;
  }
  const int new_fd = ::fcntl(fd, F_DUPFD, 4);
  if (new_fd < 0) {
    return false;
  }
  ::close(fd);
  fd = new_fd;
  return true;
}

std::string describe_exit_status(int st) {
  if (WIFSIGNALED(st)) {
    return "czcamera-cam terminated by signal " +
           std::to_string(WTERMSIG(st));
  }
  if (WIFEXITED(st)) {
    return "czcamera-cam exited unexpectedly (code " +
           std::to_string(WEXITSTATUS(st)) + ")";
  }
  return "czcamera-cam terminated unexpectedly";
}

bool reap_pid_with_timeout(pid_t pid,
                           int timeout_ms,
                           int sleep_step_ms,
                           int* out_status,
                           bool* out_got_status) {
  if (out_got_status) *out_got_status = false;
  const auto dl =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    int st = 0;
    const pid_t wr = ::waitpid(pid, &st, WNOHANG);
    if (wr == pid) {
      if (out_status) *out_status = st;
      if (out_got_status) *out_got_status = true;
      return true;
    }
    if (wr < 0) {
      if (errno == EINTR) continue;
      return errno == ECHILD;
    }
    if (std::chrono::steady_clock::now() >= dl) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_step_ms));
  }
}

uint16_t read_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint8_t clamp_yuv_channel(int v) {
  if (v <= 0) return 0;
  const int shifted = v >> 8;
  return shifted >= 255 ? 255 : static_cast<uint8_t>(shifted);
}

bool get_json_bool(const std::map<std::string, json_detail::JsonValue>& obj,
                   const char* key,
                   bool default_val = false) {
  const auto it = obj.find(key);
  if (it != obj.end() &&
      it->second.type == json_detail::JsonValue::Type::Bool) {
    return it->second.bool_val;
  }
  return default_val;
}

double get_json_num(const std::map<std::string, json_detail::JsonValue>& obj,
                    const char* key,
                    double default_val = 0.0) {
  const auto it = obj.find(key);
  if (it != obj.end() &&
      it->second.type == json_detail::JsonValue::Type::Number) {
    return it->second.num_val;
  }
  return default_val;
}

std::string get_json_str(const std::map<std::string, json_detail::JsonValue>& obj,
                         const char* key,
                         const std::string& default_val = "") {
  const auto it = obj.find(key);
  if (it != obj.end() &&
      it->second.type == json_detail::JsonValue::Type::String) {
    return it->second.str_val;
  }
  return default_val;
}

}  // namespace

bool parse_czfr_header(const uint8_t* hdr16,
                       uint16_t& w,
                       uint16_t& h,
                       uint8_t& pixfmt,
                       uint8_t& flags,
                       uint32_t& payload_bytes) {
  if (!hdr16) {
    return false;
  }
  if (hdr16[0] != 'C' || hdr16[1] != 'Z' || hdr16[2] != 'F' ||
      hdr16[3] != 'R') {
    return false;
  }
  const uint16_t parsed_w = read_le16(hdr16 + 4);
  const uint16_t parsed_h = read_le16(hdr16 + 6);
  const uint8_t parsed_fmt = hdr16[8];
  const uint8_t parsed_flags = hdr16[9];
  const uint32_t parsed_len = read_le32(hdr16 + 12);

  if (parsed_w == 0 || parsed_h == 0) {
    return false;
  }
  if (parsed_fmt != kCzfrPixfmtYuv420 && parsed_fmt != kCzfrPixfmtRgb24) {
    return false;
  }
  if (parsed_len == 0 || parsed_len > kCzfrMaxPayloadBytes) {
    return false;
  }

  w = parsed_w;
  h = parsed_h;
  pixfmt = parsed_fmt;
  flags = parsed_flags;
  payload_bytes = parsed_len;
  return true;
}

bool yuv420_to_rgb24(const uint8_t* yuv,
                     int width,
                     int height,
                     std::vector<uint8_t>& out_rgb24) {
  if (!yuv || width <= 0 || height <= 0 || (width & 1) != 0 ||
      (height & 1) != 0) {
    return false;
  }

  const size_t w = static_cast<size_t>(width);
  const size_t h = static_cast<size_t>(height);
  const size_t y_size = w * h;
  const size_t uv_w = w / 2;
  const size_t uv_h = h / 2;
  const size_t uv_size = uv_w * uv_h;

  const uint8_t* y_plane = yuv;
  const uint8_t* u_plane = yuv + y_size;
  const uint8_t* v_plane = u_plane + uv_size;

  out_rgb24.resize(y_size * 3u);

  for (size_t row = 0; row < h; ++row) {
    const uint8_t* y_row = y_plane + row * w;
    const uint8_t* u_row = u_plane + (row >> 1) * uv_w;
    const uint8_t* v_row = v_plane + (row >> 1) * uv_w;
    uint8_t* dst = out_rgb24.data() + row * w * 3u;

    for (size_t col = 0; col < w; ++col) {
      const int c = static_cast<int>(y_row[col]) - 16;
      const int d = static_cast<int>(u_row[col >> 1]) - 128;
      const int e = static_cast<int>(v_row[col >> 1]) - 128;

      const int base = 298 * c + 128;
      dst[0] = clamp_yuv_channel(base + 409 * e);
      dst[1] = clamp_yuv_channel(base - 100 * d - 208 * e);
      dst[2] = clamp_yuv_channel(base + 516 * d);
      dst += 3;
    }
  }
  return true;
}

SubprocessCameraBackend::SubprocessCameraBackend(
    std::string cam_bin_path,
    std::vector<std::string> extra_args)
    : cam_bin_path_(std::move(cam_bin_path)),
      extra_args_(std::move(extra_args)) {
  ignore_sigpipe_once();
}

SubprocessCameraBackend::~SubprocessCameraBackend() { stop(); }

std::string SubprocessCameraBackend::resolve_bin_path() const {
  if (!cam_bin_path_.empty()) {
    return expand_user_path(cam_bin_path_);
  }
  const char* env_bin = std::getenv("CZCAMERA_CAM_BIN");
  if (env_bin && *env_bin) {
    return expand_user_path(env_bin);
  }
  return "/usr/bin/czcamera-cam";
}

void SubprocessCameraBackend::close_stdin_fd() {
  std::lock_guard<std::mutex> lock(write_mutex_);
  close_fd_safe(stdin_write_fd_);
}

void SubprocessCameraBackend::cleanup_fds() {
  close_stdin_fd();
  close_fd_safe(stdout_read_fd_);
  close_fd_safe(frame_read_fd_);
  close_fd_safe(wake_read_fd_);
  close_fd_safe(wake_write_fd_);
}

void SubprocessCameraBackend::stop_locked() {
  pid_t pid = -1;
  bool can_shutdown_gracefully = false;
  int wake_fd = -1;
  {
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    if (!child_running_ && !stdout_thread_.joinable() &&
        !frame_thread_.joinable() && child_pid_ <= 0) {
      ready_ = false;
      recording_ = false;
      paused_ = false;
      return;
    }
    stopping_ = true;
    ready_ = false;
    recording_ = false;
    paused_ = false;
    pid = child_pid_;
    wake_fd = wake_write_fd_;
    can_shutdown_gracefully = !process_dead_;
    reply_cv_.notify_all();
  }

  if (can_shutdown_gracefully) {
    std::lock_guard<std::mutex> wr_lock(write_mutex_);
    if (stdin_write_fd_ >= 0) {
      const std::string line = "{\"id\":999999,\"cmd\":\"shutdown\"}\n";
      std::string ignored_err;
      write_all_fd(stdin_write_fd_, line.data(), line.size(), ignored_err);
      close_fd_safe(stdin_write_fd_);
    }
  } else {
    close_stdin_fd();
  }

  if (pid > 0) {
    std::lock_guard<std::mutex> reap_lock(reap_mutex_);
    pid_t cur_pid = -1;
    {
      std::lock_guard<std::mutex> st_lock(state_mutex_);
      cur_pid = child_pid_;
    }
    if (cur_pid > 0) {
      if (!reap_pid_with_timeout(cur_pid, kShutdownTimeoutMs / 2, 10, nullptr,
                                 nullptr)) {
        ::kill(cur_pid, SIGTERM);
        if (!reap_pid_with_timeout(cur_pid, 500, 10, nullptr, nullptr)) {
          ::kill(cur_pid, SIGKILL);
          int st = 0;
          while (::waitpid(cur_pid, &st, 0) < 0 && errno == EINTR) {
          }
        }
      }
      std::lock_guard<std::mutex> st_lock(state_mutex_);
      child_pid_ = -1;
    }
  }

  if (wake_fd >= 0) {
    const char b = 1;
    const ssize_t ignored = ::write(wake_fd, &b, 1);
    (void)ignored;
  }

  if (stdout_thread_.joinable()) {
    stdout_thread_.join();
  }
  if (frame_thread_.joinable()) {
    frame_thread_.join();
  }

  cleanup_fds();

  std::lock_guard<std::mutex> st_lock(state_mutex_);
  child_running_ = false;
  stopping_ = false;
}

bool SubprocessCameraBackend::start(const CameraStreamSpec& initial_spec,
                                    std::string& out_err) {
  std::lock_guard<std::mutex> life_lock(lifecycle_mutex_);
  out_err.clear();

  // Clean up any previous worker before spawning a fresh one.
  stop_locked();

  // Wait for any unwinding command thread before resetting state.
  std::lock_guard<std::mutex> cmd_lock(cmd_mutex_);

  {
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    child_pid_ = -1;
    child_running_ = false;
    ready_ = false;
    recording_ = false;
    paused_ = false;
    audio_warning_ = false;
    stopping_ = false;
    process_dead_ = false;
    last_error_.clear();
    backend_name_ = "picamera2-subprocess";
    ready_received_ = false;
    ready_ok_ = false;
    ready_error_.clear();
    pending_cmd_id_ = 0;
    pending_reply_ready_ = false;
    pending_reply_.clear();
  }
  {
    std::lock_guard<std::mutex> fr_lock(frame_mutex_);
    latest_raw_ = RawFrameSlot{};
  }

  const std::string bin_path = resolve_bin_path();
  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  int frm_pipe[2] = {-1, -1};
  int wk_pipe[2] = {-1, -1};

  if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0 ||
      ::pipe(frm_pipe) != 0 || ::pipe(wk_pipe) != 0) {
    out_err = std::string("pipe() failed: ") + std::strerror(errno);
    for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                   frm_pipe[0], frm_pipe[1], wk_pipe[0], wk_pipe[1]}) {
      if (fd >= 0) ::close(fd);
    }
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    last_error_ = out_err;
    return false;
  }

  for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                 frm_pipe[0], frm_pipe[1], wk_pipe[0], wk_pipe[1]}) {
    set_cloexec(fd);
  }
  set_nonblocking(out_pipe[0]);
  set_nonblocking(frm_pipe[0]);
  set_nonblocking(wk_pipe[0]);
  set_nonblocking(wk_pipe[1]);

#ifdef F_SETNOSIGPIPE
  ::fcntl(in_pipe[1], F_SETNOSIGPIPE, 1);
#endif

  const int log_fd = open_stderr_log_fd();

  // Prepare argv strings before fork() so child executes only async-signal-safe ops.
  std::vector<std::string> argv_storage;
  argv_storage.push_back(bin_path);
  argv_storage.push_back("--frame-fd");
  argv_storage.push_back(std::to_string(kChildFrameFd));
  for (const auto& arg : extra_args_) {
    argv_storage.push_back(arg);
  }
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(argv_storage.size() + 1);
  for (auto& s : argv_storage) {
    argv_ptrs.push_back(const_cast<char*>(s.c_str()));
  }
  argv_ptrs.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    out_err = std::string("fork() failed: ") + std::strerror(errno);
    if (log_fd >= 0) ::close(log_fd);
    for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                   frm_pipe[0], frm_pipe[1], wk_pipe[0], wk_pipe[1]}) {
      ::close(fd);
    }
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    last_error_ = out_err;
    return false;
  }

  if (pid == 0) {
    // Child process
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(frm_pipe[0]);
    ::close(wk_pipe[0]);
    ::close(wk_pipe[1]);

    int child_in = in_pipe[0];
    int child_out = out_pipe[1];
    int child_err = log_fd;
    int child_frm = frm_pipe[1];

    if (!move_fd_above_3(child_in) || !move_fd_above_3(child_out) ||
        !move_fd_above_3(child_err) || !move_fd_above_3(child_frm)) {
      _exit(126);
    }

    if (::dup2(child_in, STDIN_FILENO) < 0 ||
        ::dup2(child_out, STDOUT_FILENO) < 0 ||
        ::dup2(child_frm, kChildFrameFd) < 0) {
      _exit(126);
    }
    if (child_err >= 0) {
      ::dup2(child_err, STDERR_FILENO);
    }

    // Close originals and any stray descriptors above kChildFrameFd.
    if (child_in >= 4) ::close(child_in);
    if (child_out >= 4) ::close(child_out);
    if (child_err >= 4) ::close(child_err);
    if (child_frm >= 4) ::close(child_frm);

    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd <= 0 || max_fd > 4096) {
      max_fd = 1024;
    }
    for (int fd = kChildFrameFd + 1; fd < static_cast<int>(max_fd); ++fd) {
      ::close(fd);
    }

    ::execvp(bin_path.c_str(), argv_ptrs.data());
    const int err_no = errno;
    const char* msg = std::strerror(err_no);
    const char prefix[] = "czcamera: execvp failed: ";
    const ssize_t w1 = ::write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    const ssize_t w2 = ::write(STDERR_FILENO, msg, std::strlen(msg));
    const ssize_t w3 = ::write(STDERR_FILENO, "\n", 1);
    (void)w1;
    (void)w2;
    (void)w3;
    _exit(127);
  }

  // Parent process
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  ::close(frm_pipe[1]);
  if (log_fd >= 0) {
    ::close(log_fd);
  }

  {
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    child_pid_ = pid;
    child_running_ = true;
  }
  {
    std::lock_guard<std::mutex> wr_lock(write_mutex_);
    stdin_write_fd_ = in_pipe[1];
  }
  stdout_read_fd_ = out_pipe[0];
  frame_read_fd_ = frm_pipe[0];
  wake_read_fd_ = wk_pipe[0];
  wake_write_fd_ = wk_pipe[1];

  stdout_thread_ = std::thread(&SubprocessCameraBackend::stdout_reader_loop,
                               this, stdout_read_fd_, wake_read_fd_, pid);
  frame_thread_ = std::thread(&SubprocessCameraBackend::frame_reader_loop, this,
                              frame_read_fd_, wake_read_fd_);

  // Wait for {"event":"ready",...}
  bool got_ready = false;
  bool ready_ok = false;
  std::string ready_err;
  {
    std::unique_lock<std::mutex> st_lock(state_mutex_);
    got_ready = reply_cv_.wait_for(
        st_lock, std::chrono::milliseconds(kReadyTimeoutMs),
        [&]() { return ready_received_ || process_dead_ || stopping_; });
    ready_ok = ready_ok_;
    ready_err = ready_error_;
    if (!got_ready) {
      ready_err = "timed out waiting for camera helper ready event";
    } else if (process_dead_ && ready_err.empty()) {
      ready_err = last_error_.empty() ? "camera helper exited before ready"
                                      : last_error_;
    }
  }

  if (!got_ready || !ready_ok) {
    out_err = ready_err.empty() ? "camera helper failed to initialize"
                                : ready_err;
    stop_locked();
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    last_error_ = out_err;
    return false;
  }

  if (!send_configure(initial_spec, out_err, true)) {
    stop_locked();
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    last_error_ = out_err;
    return false;
  }

  std::lock_guard<std::mutex> st_lock(state_mutex_);
  ready_ = true;
  paused_ = false;
  recording_ = false;
  last_error_.clear();
  return true;
}

void SubprocessCameraBackend::stop() {
  std::lock_guard<std::mutex> life_lock(lifecycle_mutex_);
  stop_locked();
}

bool SubprocessCameraBackend::send_command(
    const std::string& cmd_name,
    const std::string& json_body_without_braces,
    int timeout_ms,
    std::map<std::string, json_detail::JsonValue>& out_reply,
    std::string& out_err,
    bool allow_before_ready) {
  out_err.clear();
  out_reply.clear();

  uint64_t cmd_id = 0;
  {
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    if (stopping_ || process_dead_ || !child_running_) {
      out_err = last_error_.empty() ? "camera helper is not running"
                                    : last_error_;
      return false;
    }
    if (!allow_before_ready && !ready_) {
      out_err = last_error_.empty() ? "camera backend not ready" : last_error_;
      return false;
    }
    cmd_id = next_cmd_id_++;
    pending_cmd_id_ = cmd_id;
    pending_reply_ready_ = false;
    pending_reply_.clear();
  }

  std::ostringstream oss;
  oss.imbue(std::locale::classic());
  oss << "{\"id\":" << cmd_id << ",\"cmd\":\""
      << json_detail::escape_json_string(cmd_name) << "\"";
  if (!json_body_without_braces.empty()) {
    oss << "," << json_body_without_braces;
  }
  oss << "}\n";
  const std::string line = oss.str();

  {
    std::lock_guard<std::mutex> wr_lock(write_mutex_);
    if (stdin_write_fd_ < 0) {
      out_err = "camera helper stdin is closed";
      std::lock_guard<std::mutex> st_lock(state_mutex_);
      pending_cmd_id_ = 0;
      return false;
    }
    if (!write_all_fd(stdin_write_fd_, line.data(), line.size(), out_err)) {
      std::lock_guard<std::mutex> st_lock(state_mutex_);
      pending_cmd_id_ = 0;
      if (last_error_.empty()) {
        last_error_ = out_err;
      }
      return false;
    }
  }

  std::unique_lock<std::mutex> st_lock(state_mutex_);
  const bool woke = reply_cv_.wait_for(
      st_lock, std::chrono::milliseconds(timeout_ms), [&]() {
        return pending_reply_ready_ || process_dead_ || stopping_;
      });

  pending_cmd_id_ = 0;
  if (!woke) {
    out_err = "camera helper command '" + cmd_name + "' timed out";
    last_error_ = out_err;
    return false;
  }
  if (stopping_) {
    out_err = "camera backend stopping";
    return false;
  }
  if (process_dead_ && !pending_reply_ready_) {
    out_err = last_error_.empty() ? "camera helper exited unexpectedly"
                                  : last_error_;
    return false;
  }

  out_reply = std::move(pending_reply_);
  pending_reply_ready_ = false;

  if (!get_json_bool(out_reply, "ok", false)) {
    const std::string err = get_json_str(out_reply, "error");
    out_err = err.empty() ? ("camera helper command '" + cmd_name + "' failed")
                          : err;
    last_error_ = out_err;
    return false;
  }
  return true;
}

bool SubprocessCameraBackend::send_configure(const CameraStreamSpec& spec,
                                             std::string& out_err,
                                             bool allow_before_ready) {
  const std::string mode = spec.mode.empty() ? "photo" : spec.mode;
  const std::string transform =
      spec.camera_transform.empty() ? "auto" : spec.camera_transform;
  const float zoom = std::clamp(spec.zoom, 1.0f, 4.0f);
  const float ev = std::clamp(spec.ev, -2.0f, 2.0f);

  std::ostringstream body;
  body.imbue(std::locale::classic());
  body << std::fixed << std::setprecision(2);
  body << "\"mode\":\"" << json_detail::escape_json_string(mode)
       << "\",\"main_w\":" << spec.main_width
       << ",\"main_h\":" << spec.main_height
       << ",\"preview_w\":" << spec.preview_width
       << ",\"preview_h\":" << spec.preview_height
       << ",\"fps\":" << spec.fps << ",\"zoom\":" << zoom
       << ",\"ev\":" << ev << ",\"transform\":\""
       << json_detail::escape_json_string(transform) << "\"";

  std::map<std::string, json_detail::JsonValue> reply;
  if (!send_command("configure", body.str(), kCommandTimeoutMs, reply, out_err,
                    allow_before_ready)) {
    return false;
  }
  std::lock_guard<std::mutex> st_lock(state_mutex_);
  current_spec_ = spec;
  current_spec_.mode = mode;
  current_spec_.camera_transform = transform;
  current_spec_.zoom = zoom;
  current_spec_.ev = ev;
  paused_ = false;
  return true;
}

bool SubprocessCameraBackend::reconfigure(const CameraStreamSpec& spec,
                                          std::string& out_err) {
  std::lock_guard<std::mutex> cmd_lock(cmd_mutex_);
  return send_configure(spec, out_err);
}

bool SubprocessCameraBackend::set_controls(float zoom,
                                             float ev,
                                             std::string& out_err) {
  std::lock_guard<std::mutex> cmd_lock(cmd_mutex_);
  const float clamped_zoom = std::clamp(zoom, 1.0f, 4.0f);
  const float clamped_ev = std::clamp(ev, -2.0f, 2.0f);

  std::ostringstream body;
  body.imbue(std::locale::classic());
  body << std::fixed << std::setprecision(2);
  body << "\"zoom\":" << clamped_zoom << ",\"ev\":" << clamped_ev;

  std::map<std::string, json_detail::JsonValue> reply;
  if (!send_command("set_controls", body.str(), kCommandTimeoutMs, reply,
                    out_err)) {
    return false;
  }
  std::lock_guard<std::mutex> st_lock(state_mutex_);
  current_spec_.zoom = clamped_zoom;
  current_spec_.ev = clamped_ev;
  return true;
}

bool SubprocessCameraBackend::pause(std::string& out_err) {
  std::lock_guard<std::mutex> cmd_lock(cmd_mutex_);
  std::map<std::string, json_detail::JsonValue> reply;
  if (!send_command("pause", "", kCommandTimeoutMs, reply, out_err)) {
    return false;
  }
  std::lock_guard<std::mutex> st_lock(state_mutex_);
  paused_ = true;
  recording_ = false;
  return true;
}

bool SubprocessCameraBackend::resume(std::string& out_err) {
  std::lock_guard<std::mutex> cmd_lock(cmd_mutex_);
  std::map<std::string, json_detail::JsonValue> reply;
  if (!send_command("resume", "", kCommandTimeoutMs, reply, out_err)) {
    return false;
  }
  std::lock_guard<std::mutex> st_lock(state_mutex_);
  paused_ = false;
  return true;
}

bool SubprocessCameraBackend::copy_latest_preview(uint64_t last_seen_seq,
                                                  PreviewFrame& out_frame) {
  RawFrameSlot snap;
  {
    std::lock_guard<std::mutex> fr_lock(frame_mutex_);
    if (latest_raw_.seq == 0 || latest_raw_.seq <= last_seen_seq) {
      return false;
    }
    snap = latest_raw_;
  }

  out_frame.seq = snap.seq;
  out_frame.width = static_cast<int>(snap.width);
  out_frame.height = static_cast<int>(snap.height);
  out_frame.is_16_9 = (snap.flags & kCzfrFlagAspect16_9) != 0;

  if (snap.pixfmt == kCzfrPixfmtRgb24) {
    out_frame.rgb24 = std::move(snap.payload);
    return true;
  }
  if (snap.pixfmt == kCzfrPixfmtYuv420) {
    return yuv420_to_rgb24(snap.payload.data(), out_frame.width,
                           out_frame.height, out_frame.rgb24);
  }
  return false;
}

StillCaptureResult SubprocessCameraBackend::capture_still(
    const std::string& tmp_jpeg_path,
    int width,
    int height) {
  StillCaptureResult res;
  if (tmp_jpeg_path.empty() || width <= 0 || height <= 0) {
    res.error = "invalid capture_still arguments";
    return res;
  }

  std::lock_guard<std::mutex> cmd_lock(cmd_mutex_);
  std::ostringstream body;
  body.imbue(std::locale::classic());
  body << "\"tmp_path\":\"" << json_detail::escape_json_string(tmp_jpeg_path)
       << "\",\"width\":" << width << ",\"height\":" << height;

  std::map<std::string, json_detail::JsonValue> reply;
  if (!send_command("capture_still", body.str(), kCommandTimeoutMs, reply,
                    res.error)) {
    return res;
  }

  res.ok = true;
  res.width = static_cast<int>(std::llround(get_json_num(reply, "width", width)));
  res.height =
      static_cast<int>(std::llround(get_json_num(reply, "height", height)));
  const double b = get_json_num(reply, "bytes", 0.0);
  res.bytes = b > 0.0 ? static_cast<uint64_t>(std::llround(b)) : 0ULL;
  return res;
}

bool SubprocessCameraBackend::start_video(const VideoSpec& spec,
                                          std::string& out_err) {
  if (spec.tmp_path.empty() || spec.width <= 0 || spec.height <= 0 ||
      spec.fps <= 0) {
    out_err = "invalid start_video specification";
    return false;
  }

  std::lock_guard<std::mutex> cmd_lock(cmd_mutex_);
  {
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    audio_warning_ = false;
  }

  std::ostringstream body;
  body.imbue(std::locale::classic());
  body << "\"tmp_path\":\"" << json_detail::escape_json_string(spec.tmp_path)
       << "\",\"width\":" << spec.width << ",\"height\":" << spec.height
       << ",\"fps\":" << spec.fps << ",\"bitrate\":" << spec.bitrate_bps
       << ",\"audio\":" << (spec.audio_enabled ? "true" : "false");

  std::map<std::string, json_detail::JsonValue> reply;
  if (!send_command("start_video", body.str(), kCommandTimeoutMs, reply,
                    out_err)) {
    return false;
  }

  std::lock_guard<std::mutex> st_lock(state_mutex_);
  recording_ = true;
  if (spec.audio_enabled && !get_json_bool(reply, "audio_recorded", false)) {
    audio_warning_ = true;
  }
  return true;
}

VideoStopResult SubprocessCameraBackend::stop_video() {
  VideoStopResult res;
  std::lock_guard<std::mutex> cmd_lock(cmd_mutex_);

  std::map<std::string, json_detail::JsonValue> reply;
  if (!send_command("stop_video", "", kStopVideoTimeoutMs, reply, res.error)) {
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    recording_ = false;
    return res;
  }

  {
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    recording_ = false;
  }

  res.ok = true;
  res.duration_sec = get_json_num(reply, "duration_sec", 0.0);
  const double b = get_json_num(reply, "bytes", 0.0);
  res.bytes = b > 0.0 ? static_cast<uint64_t>(std::llround(b)) : 0ULL;
  res.audio_recorded = get_json_bool(reply, "audio_recorded", false);
  return res;
}

BackendHealth SubprocessCameraBackend::status() const {
  std::lock_guard<std::mutex> st_lock(state_mutex_);
  BackendHealth h;
  h.ready = ready_;
  h.recording = recording_;
  h.paused = paused_;
  h.audio_warning = audio_warning_;
  h.last_error = last_error_;
  h.backend_name = backend_name_;
  return h;
}

pid_t SubprocessCameraBackend::child_pid() const {
  std::lock_guard<std::mutex> st_lock(state_mutex_);
  return child_pid_;
}

void SubprocessCameraBackend::handle_stdout_line(const std::string& line) {
  std::map<std::string, json_detail::JsonValue> obj;
  if (!json_detail::parse_json_object(line, obj)) {
    return;
  }

  const std::string event = get_json_str(obj, "event");
  if (!event.empty()) {
    if (event == "ready") {
      const double proto = get_json_num(obj, "protocol", 1.0);
      std::lock_guard<std::mutex> st_lock(state_mutex_);
      ready_received_ = true;
      if (static_cast<int>(std::llround(proto)) != 1) {
        ready_ok_ = false;
        ready_error_ = "unsupported camera helper protocol version";
      } else {
        ready_ok_ = true;
        ready_error_.clear();
      }
      reply_cv_.notify_all();
      return;
    }
    if (event == "warn") {
      const std::string code = get_json_str(obj, "code");
      const std::string msg = get_json_str(obj, "message");
      std::lock_guard<std::mutex> st_lock(state_mutex_);
      if (code == "audio_unavailable") {
        audio_warning_ = true;
      }
      if (!msg.empty()) {
        last_error_ = msg;
      }
      return;
    }
    return;
  }

  const auto id_it = obj.find("id");
  if (id_it != obj.end() &&
      id_it->second.type == json_detail::JsonValue::Type::Number) {
    const uint64_t reply_id =
        static_cast<uint64_t>(std::llround(id_it->second.num_val));
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    if (pending_cmd_id_ != 0 && reply_id == pending_cmd_id_) {
      pending_reply_ = std::move(obj);
      pending_reply_ready_ = true;
      reply_cv_.notify_all();
    }
  }
}

void SubprocessCameraBackend::stdout_reader_loop(int stdout_fd,
                                                   int wake_fd,
                                                   pid_t pid) {
  std::string line_buf;
  std::array<char, 4096> chunk{};
  bool eof_seen = false;

  while (!eof_seen) {
    pollfd pfds[2] = {
        {stdout_fd, POLLIN, 0},
        {wake_fd, POLLIN, 0},
    };
    const int rc = ::poll(pfds, 2, -1);
    if (rc < 0) {
      if (errno == EINTR) continue;
      eof_seen = true;
      break;
    }
    if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      break;
    }
    if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      while (true) {
        const ssize_t r = ::read(stdout_fd, chunk.data(), chunk.size());
        if (r > 0) {
          line_buf.append(chunk.data(), static_cast<size_t>(r));
          size_t nl = 0;
          while ((nl = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, nl);
            line_buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') {
              line.pop_back();
            }
            if (!line.empty()) {
              handle_stdout_line(line);
            }
          }
          continue;
        }
        if (r == 0) {
          if (!line_buf.empty()) {
            if (line_buf.back() == '\r') line_buf.pop_back();
            if (!line_buf.empty()) handle_stdout_line(line_buf);
            line_buf.clear();
          }
          eof_seen = true;
          break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        eof_seen = true;
        break;
      }
    }
  }

  bool was_stopping = false;
  {
    std::lock_guard<std::mutex> st_lock(state_mutex_);
    was_stopping = stopping_;
  }

  std::string reason = "czcamera-cam stdout closed unexpectedly";
  bool reaped_in_reader = false;
  if (!was_stopping && pid > 0) {
    std::lock_guard<std::mutex> reap_lock(reap_mutex_);
    bool still_ours = false;
    {
      std::lock_guard<std::mutex> st_lock(state_mutex_);
      still_ours = (child_pid_ == pid && !stopping_);
    }
    if (still_ours) {
      // Reap unexpected child termination quickly so no zombie PID lingers.
      int st = 0;
      bool got_status = false;
      reaped_in_reader = reap_pid_with_timeout(pid, 200, 5, &st, &got_status);
      if (got_status) {
        reason = describe_exit_status(st);
      }
    }
  }

  std::lock_guard<std::mutex> st_lock(state_mutex_);
  if (reaped_in_reader && child_pid_ == pid) {
    child_pid_ = -1;
  }
  process_dead_ = true;
  ready_ = false;
  recording_ = false;
  if (!was_stopping) {
    last_error_ = reason;
    if (!ready_received_) {
      ready_received_ = true;
      ready_ok_ = false;
      ready_error_ = reason;
    }
  }
  reply_cv_.notify_all();
}

void SubprocessCameraBackend::frame_reader_loop(int frame_fd, int wake_fd) {
  std::array<uint8_t, kCzfrHeaderBytes> hdr{};
  RawFrameSlot staging;

  while (true) {
    bool woken = false;
    bool eof = false;
    if (!read_exact_with_wake(frame_fd, wake_fd, hdr.data(), hdr.size(), woken,
                              eof)) {
      break;
    }

    uint16_t w = 0;
    uint16_t h = 0;
    uint8_t pixfmt = 0;
    uint8_t flags = 0;
    uint32_t payload_bytes = 0;
    if (!parse_czfr_header(hdr.data(), w, h, pixfmt, flags, payload_bytes)) {
      std::lock_guard<std::mutex> st_lock(state_mutex_);
      if (!stopping_ && last_error_.empty()) {
        last_error_ = "invalid CZFR frame header";
      }
      break;
    }

    size_t expected_bytes = 0;
    if (pixfmt == kCzfrPixfmtYuv420) {
      if ((w & 1u) != 0 || (h & 1u) != 0) {
        break;
      }
      expected_bytes =
          (static_cast<size_t>(w) * static_cast<size_t>(h) * 3u) / 2u;
    } else if (pixfmt == kCzfrPixfmtRgb24) {
      expected_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3u;
    }
    if (payload_bytes != expected_bytes) {
      std::lock_guard<std::mutex> st_lock(state_mutex_);
      if (!stopping_ && last_error_.empty()) {
        last_error_ = "CZFR payload length mismatch";
      }
      break;
    }

    staging.payload.resize(payload_bytes);
    if (!read_exact_with_wake(frame_fd, wake_fd, staging.payload.data(),
                              payload_bytes, woken, eof)) {
      break;
    }

    staging.width = w;
    staging.height = h;
    staging.pixfmt = pixfmt;
    staging.flags = flags;
    staging.seq = ++frame_seq_counter_;

    std::lock_guard<std::mutex> fr_lock(frame_mutex_);
    std::swap(latest_raw_, staging);
  }
}

}  // namespace cz
