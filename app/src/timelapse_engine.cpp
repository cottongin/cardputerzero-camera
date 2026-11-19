#include "timelapse_engine.hpp"

#include "video_player.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace cz {

namespace {

std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string parent_dir_of(const std::string& path) {
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') {
    p.pop_back();
  }
  const size_t slash = p.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return p.substr(0, slash);
}

std::string current_iso8601_utc() {
  const std::time_t now = std::time(nullptr);
  struct tm tm_utc {};
  ::gmtime_r(&now, &tm_utc);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
  return std::string(buf);
}

void resolution_dimensions(const std::string& resolution, int& out_w, int& out_h) {
  const std::string res = to_lower_ascii(resolution);
  if (res == "8mp") {
    out_w = 3280;
    out_h = 2464;
  } else if (res == "1080p") {
    out_w = 1920;
    out_h = 1080;
  } else {
    out_w = 1640;
    out_h = 1232;
  }
}

int count_valid_jpegs(const std::string& dir_path) {
  DIR* d = ::opendir(dir_path.c_str());
  if (!d) {
    return 0;
  }
  int count = 0;
  struct dirent* ent = nullptr;
  while ((ent = ::readdir(d)) != nullptr) {
    const std::string name(ent->d_name);
    if (name.empty() || name[0] == '.') {
      continue;
    }
    if (!media_detail::ends_with_ci(name, ".jpg") &&
        !media_detail::ends_with_ci(name, ".jpeg")) {
      continue;
    }
    const std::string full = dir_path + "/" + name;
    struct stat st {};
    if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > 0) {
      ++count;
    }
  }
  ::closedir(d);
  return count;
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

void remove_silent(const std::string& path) {
  if (path.empty()) return;
  std::error_code ec;
  fs::remove(path, ec);
}

}  // namespace

TimelapseEngine::TimelapseEngine(ICameraBackend* backend) : backend_(backend) {
  media_detail::ignore_sigpipe_once();
}

TimelapseEngine::~TimelapseEngine() {
  cancel_compile();
  std::lock_guard<std::mutex> lock(mutex_);
  state_.running = false;
}

void TimelapseEngine::set_backend(ICameraBackend* backend) {
  std::lock_guard<std::mutex> lock(mutex_);
  backend_ = backend;
}

std::vector<std::string> TimelapseEngine::build_compile_command(
    const TimelapseSessionPaths& paths,
    int fps,
    const std::string& resolution,
    bool use_hw_encoder) {
  const int safe_fps = std::clamp(fps, 1, 120);
  std::string input_pattern = paths.work_dir;
  if (!input_pattern.empty() && input_pattern.back() != '/') {
    input_pattern += '/';
  }
  input_pattern += "frame_%04d.jpg";

  std::vector<std::string> args = {
      "ffmpeg",
      "-nostdin",
      "-y",
      "-framerate",
      std::to_string(safe_fps),
      "-start_number",
      "1",
      "-i",
      input_pattern,
  };

  if (to_lower_ascii(resolution) == "8mp") {
    args.push_back("-vf");
    args.push_back("scale=1440:1080:flags=lanczos");
  }

  if (use_hw_encoder) {
    args.push_back("-c:v");
    args.push_back("h264_v4l2m2m");
    args.push_back("-b:v");
    args.push_back("6M");
    args.push_back("-pix_fmt");
    args.push_back("yuv420p");
  } else {
    args.push_back("-c:v");
    args.push_back("libx264");
    args.push_back("-preset");
    args.push_back("veryfast");
    args.push_back("-crf");
    args.push_back("22");
    args.push_back("-pix_fmt");
    args.push_back("yuv420p");
  }

  args.push_back("-f");
  args.push_back("mp4");
  args.push_back("-movflags");
  args.push_back("+faststart");
  args.push_back("-loglevel");
  args.push_back("error");
  args.push_back("-progress");
  args.push_back("pipe:1");
  args.push_back(paths.mp4_tmp_path);
  return args;
}

bool TimelapseEngine::start(const AppConfig& cfg, uint64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_.running || state_.compiling) {
    state_.last_error = "Timelapse already active";
    return false;
  }
  if (!backend_) {
    state_.last_error = "No camera backend available";
    return false;
  }
  const BackendHealth health = backend_->status();
  if (!health.ready || health.paused) {
    state_.last_error = "Camera backend is not ready";
    return false;
  }

  const std::string out_dir = expand_user_path(cfg.output_dir);
  if (out_dir.empty() || !mkdir_p(out_dir)) {
    state_.last_error = "Failed to prepare output directory";
    return false;
  }
  if (!has_sufficient_storage(out_dir)) {
    state_.last_error = "Insufficient storage (<100MB free)";
    return false;
  }

  const bool is_photos = (to_lower_ascii(cfg.timelapse_output_mode) == "photos");
  TimelapseSessionPaths paths = create_timelapse_session(out_dir, !is_photos);
  if (is_photos) {
    if (!mkdir_p(paths.photos_dir)) {
      state_.last_error = "Failed to create timelapse folder";
      return false;
    }
  } else {
    if (!mkdir_p(paths.work_dir)) {
      state_.last_error = "Failed to create timelapse work directory";
      return false;
    }
  }

  const std::string res =
      cfg.timelapse_resolution.empty() ? "2mp" : to_lower_ascii(cfg.timelapse_resolution);
  const int interval_sec = std::clamp(cfg.timelapse_interval_sec, 1, 3600);
  const int fps = (cfg.timelapse_fps > 0) ? cfg.timelapse_fps : 24;

  TimelapseManifest m;
  m.session_id = paths.session_id;
  m.work_dir = paths.work_dir;
  m.created_at = current_iso8601_utc();
  m.resolution = res;
  m.interval_sec = interval_sec;
  m.fps = fps;
  m.frame_count = 0;
  m.status = "recording";

  if (!is_photos && !write_timelapse_manifest(paths.manifest_path, m)) {
    std::error_code ec;
    fs::remove_all(paths.work_dir, ec);
    state_.last_error = "Failed to write timelapse manifest";
    return false;
  }

  session_ = paths;
  manifest_ = m;
  output_dir_ = out_dir;
  resolution_ = res;
  start_ms_ = now_ms;
  next_shot_ms_ = now_ms;
  capturing_ = false;
  stop_requested_ = false;

  state_ = TimelapseState{};
  state_.running = true;
  state_.compiling = false;
  state_.shots_taken = 0;
  state_.stop_mode =
      cfg.timelapse_stop_mode.empty() ? "manual" : to_lower_ascii(cfg.timelapse_stop_mode);
  state_.stop_value = std::max(0, cfg.timelapse_stop_value);
  state_.interval_sec = interval_sec;
  state_.next_shot_in_sec = 0;
  state_.elapsed_sec = 0;
  state_.output_mode = is_photos ? "photos" : "video";
  state_.needs_compile = false;
  state_.stopped_by_low_storage = false;
  return true;
}

void TimelapseEngine::finish_capture_locked() {
  state_.running = false;
  state_.next_shot_in_sec = 0;

  if (state_.output_mode == "photos") {
    state_.needs_compile = false;
    if (state_.shots_taken > 0) {
      state_.result_path = session_.photos_dir;
    } else {
      std::error_code ec;
      fs::remove_all(session_.photos_dir, ec);
      state_.result_path.clear();
    }
  } else {
    if (state_.shots_taken > 0) {
      state_.needs_compile = true;
      manifest_.frame_count = state_.shots_taken;
      manifest_.status = "stopped";
      write_timelapse_manifest(session_.manifest_path, manifest_);
    } else {
      state_.needs_compile = false;
      std::error_code ec;
      fs::remove_all(session_.work_dir, ec);
      state_.result_path.clear();
    }
  }
}

void TimelapseEngine::tick(uint64_t now_ms, const AppConfig& /*cfg*/) {
  ICameraBackend* backend = nullptr;
  StagedPaths frame_paths;
  int cap_w = 1640;
  int cap_h = 1232;
  int next_idx = 1;
  uint64_t clamped_now = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.running || capturing_) {
      return;
    }
    clamped_now = std::max(now_ms, start_ms_);
    state_.elapsed_sec = static_cast<int>((clamped_now - start_ms_) / 1000ULL);

    if (state_.stop_mode == "duration" && state_.stop_value > 0 &&
        state_.elapsed_sec >= state_.stop_value) {
      finish_capture_locked();
      return;
    }

    if (clamped_now < next_shot_ms_) {
      state_.next_shot_in_sec =
          static_cast<int>((next_shot_ms_ - clamped_now + 999ULL) / 1000ULL);
      return;
    }

    if (!has_sufficient_storage(output_dir_)) {
      state_.stopped_by_low_storage = true;
      state_.last_error = "Low storage (<100MB)";
      finish_capture_locked();
      return;
    }

    backend = backend_;
    if (!backend) {
      state_.last_error = "Camera backend unavailable";
      finish_capture_locked();
      return;
    }

    next_idx = state_.shots_taken + 1;
    const std::string target_dir =
        (state_.output_mode == "photos") ? session_.photos_dir : session_.work_dir;
    frame_paths = next_timelapse_frame_paths(target_dir, next_idx);
    resolution_dimensions(resolution_, cap_w, cap_h);
    capturing_ = true;
  }

  const StillCaptureResult cap =
      backend->capture_still(frame_paths.tmp_path, cap_w, cap_h);

  std::lock_guard<std::mutex> lock(mutex_);
  capturing_ = false;
  if (!state_.running) {
    remove_silent(frame_paths.tmp_path);
    return;
  }

  if (!cap.ok) {
    remove_silent(frame_paths.tmp_path);
    state_.last_error =
        cap.error.empty() ? "timelapse capture_still failed" : cap.error;
    finish_capture_locked();
    return;
  }

  std::string commit_err;
  if (!commit_staged_file(frame_paths.tmp_path, frame_paths.final_path,
                          commit_err)) {
    remove_silent(frame_paths.tmp_path);
    state_.last_error = commit_err;
    finish_capture_locked();
    return;
  }

  state_.shots_taken = next_idx;
  if (state_.output_mode == "video") {
    manifest_.frame_count = state_.shots_taken;
    write_timelapse_manifest(session_.manifest_path, manifest_);
  }

  const uint64_t interval_ms =
      static_cast<uint64_t>(std::max(1, state_.interval_sec)) * 1000ULL;
  next_shot_ms_ += interval_ms;
  while (next_shot_ms_ <= clamped_now) {
    next_shot_ms_ += interval_ms;
  }
  state_.next_shot_in_sec =
      static_cast<int>((next_shot_ms_ - clamped_now + 999ULL) / 1000ULL);

  if (stop_requested_ ||
      (state_.stop_mode == "shots" && state_.stop_value > 0 &&
       state_.shots_taken >= state_.stop_value)) {
    finish_capture_locked();
  }
}

void TimelapseEngine::request_stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!state_.running) {
    return;
  }
  stop_requested_ = true;
  if (!capturing_) {
    finish_capture_locked();
  }
}

bool TimelapseEngine::run_compile_attempt(
    const std::string& ffmpeg_exe,
    const TimelapseSessionPaths& paths,
    int fps,
    const std::string& resolution,
    bool use_hw_encoder,
    int total_frames,
    const std::function<void(int, int)>& on_progress,
    std::string& out_err) {
  out_err.clear();

  std::vector<std::string> args =
      build_compile_command(paths, fps, resolution, use_hw_encoder);
  if (!args.empty()) {
    args[0] = ffmpeg_exe;
  }

  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  int wake_pipe[2] = {-1, -1};
  if (::pipe(out_pipe) != 0) {
    out_err = "pipe failed";
    return false;
  }
  if (::pipe(err_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    out_err = "pipe failed";
    return false;
  }
  if (::pipe(wake_pipe) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    out_err = "pipe failed";
    return false;
  }

  set_cloexec(out_pipe[0]);
  set_cloexec(err_pipe[0]);
  set_cloexec(wake_pipe[0]);
  set_cloexec(wake_pipe[1]);
  set_nonblocking(out_pipe[0]);
  set_nonblocking(err_pipe[0]);
  set_nonblocking(wake_pipe[0]);
  set_nonblocking(wake_pipe[1]);

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args.size() + 1);
  for (auto& a : args) {
    argv_ptrs.push_back(const_cast<char*>(a.c_str()));
  }
  argv_ptrs.push_back(nullptr);

  {
    std::lock_guard<std::mutex> plock(compile_proc_mutex_);
    if (cancel_compile_requested_) {
      ::close(out_pipe[0]);
      ::close(out_pipe[1]);
      ::close(err_pipe[0]);
      ::close(err_pipe[1]);
      ::close(wake_pipe[0]);
      ::close(wake_pipe[1]);
      out_err = "Compile cancelled";
      return false;
    }
    cancel_wake_r_ = wake_pipe[0];
    cancel_wake_w_ = wake_pipe[1];
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::lock_guard<std::mutex> plock(compile_proc_mutex_);
    cancel_wake_r_ = -1;
    cancel_wake_w_ = -1;
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    ::close(wake_pipe[0]);
    ::close(wake_pipe[1]);
    out_err = "fork failed";
    return false;
  }
  if (pid == 0) {
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);
    ::close(wake_pipe[0]);
    ::close(wake_pipe[1]);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    if (out_pipe[1] > 2) ::close(out_pipe[1]);
    if (err_pipe[1] > 2) ::close(err_pipe[1]);
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      if (devnull > 2) ::close(devnull);
    }
    ::execvp(ffmpeg_exe.c_str(), argv_ptrs.data());
    _exit(127);
  }

  ::close(out_pipe[1]);
  ::close(err_pipe[1]);

  bool cancelled_before_register = false;
  {
    std::lock_guard<std::mutex> plock(compile_proc_mutex_);
    compile_pid_ = pid;
    if (cancel_compile_requested_) {
      cancelled_before_register = true;
    }
  }
  if (cancelled_before_register) {
    ::kill(pid, SIGTERM);
  }

  std::string out_line_buf;
  std::string err_text;
  bool out_open = true;
  bool err_open = true;
  bool cancelled = cancelled_before_register;
  int last_reported = -1;

  auto handle_progress_line = [&](const std::string& raw_line) {
    std::string line = raw_line;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("frame=", 0) == 0) {
      const int frame_num = std::atoi(line.substr(6).c_str());
      const int cur = std::clamp(frame_num, 0, total_frames);
      if (cur != last_reported) {
        last_reported = cur;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          state_.compile_current = cur;
          state_.compile_total = total_frames;
          state_.compile_progress =
              total_frames > 0
                  ? static_cast<float>(cur) / static_cast<float>(total_frames)
                  : 0.0f;
        }
        if (on_progress) {
          on_progress(cur, total_frames);
        }
      }
    } else if (line == "progress=end" && total_frames > 0 &&
               last_reported < total_frames) {
      last_reported = total_frames;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.compile_current = total_frames;
        state_.compile_total = total_frames;
        state_.compile_progress = 1.0f;
      }
      if (on_progress) {
        on_progress(total_frames, total_frames);
      }
    }
  };

  while (out_open || err_open) {
    pollfd pfds[3]{};
    nfds_t nfds = 0;
    int out_idx = -1;
    int err_idx = -1;
    int wake_idx = -1;

    if (out_open) {
      out_idx = static_cast<int>(nfds);
      pfds[nfds++] = {out_pipe[0], POLLIN, 0};
    }
    if (err_open) {
      err_idx = static_cast<int>(nfds);
      pfds[nfds++] = {err_pipe[0], POLLIN, 0};
    }
    wake_idx = static_cast<int>(nfds);
    pfds[nfds++] = {wake_pipe[0], POLLIN, 0};

    const int rc = ::poll(pfds, nfds, 250);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (wake_idx >= 0 && (pfds[wake_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
      cancelled = true;
      break;
    }

    if (out_idx >= 0 && (pfds[out_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
      char buf[1024];
      while (true) {
        const ssize_t r = ::read(out_pipe[0], buf, sizeof(buf));
        if (r > 0) {
          out_line_buf.append(buf, static_cast<size_t>(r));
          size_t nl = 0;
          while ((nl = out_line_buf.find('\n')) != std::string::npos) {
            const std::string line = out_line_buf.substr(0, nl);
            out_line_buf.erase(0, nl + 1);
            handle_progress_line(line);
          }
          continue;
        }
        if (r == 0) {
          if (!out_line_buf.empty()) {
            handle_progress_line(out_line_buf);
            out_line_buf.clear();
          }
          out_open = false;
          break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        out_open = false;
        break;
      }
    }

    if (err_idx >= 0 && (pfds[err_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
      char buf[1024];
      while (true) {
        const ssize_t r = ::read(err_pipe[0], buf, sizeof(buf));
        if (r > 0) {
          if (err_text.size() < 8192) {
            err_text.append(buf, static_cast<size_t>(r));
          }
          continue;
        }
        if (r == 0) {
          err_open = false;
          break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        err_open = false;
        break;
      }
    }
  }

  ::close(out_pipe[0]);
  ::close(err_pipe[0]);

  int status = 0;
  {
    std::lock_guard<std::mutex> plock(compile_proc_mutex_);
    if (cancel_compile_requested_) {
      cancelled = true;
    }
    if (cancelled) {
      media_detail::kill_and_reap(compile_pid_);
    } else {
      const pid_t active = compile_pid_;
      compile_pid_ = -1;
      if (!media_detail::reap_pid_timeout(active, 1000, &status)) {
        pid_t tmp = active;
        media_detail::kill_and_reap(tmp);
        out_err = "ffmpeg compile timed out while exiting";
        close_fd_safe(cancel_wake_r_);
        close_fd_safe(cancel_wake_w_);
        return false;
      }
    }
    close_fd_safe(cancel_wake_r_);
    close_fd_safe(cancel_wake_w_);
  }

  if (cancelled) {
    out_err = "Compile cancelled";
    return false;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    while (!err_text.empty() &&
           (err_text.back() == '\n' || err_text.back() == '\r')) {
      err_text.pop_back();
    }
    out_err = err_text.empty() ? "ffmpeg exited with error" : err_text;
    return false;
  }

  struct stat st {};
  if (::stat(paths.mp4_tmp_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_size <= 0) {
    out_err = "compiled MP4 staging file missing or empty";
    return false;
  }

  return true;
}

bool TimelapseEngine::compile_session(
    const TimelapseSessionPaths& paths,
    int fps,
    const std::string& resolution,
    std::function<void(int, int)> on_progress) {
  std::lock_guard<std::mutex> compile_guard(compile_mutex_);
  const int total_frames = count_valid_jpegs(paths.work_dir);
  if (total_frames <= 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.compiling = false;
    state_.last_error = "No valid timelapse frames found";
    return false;
  }

  const std::string out_dir = parent_dir_of(paths.mp4_final_path);
  if (!mkdir_p(out_dir) || !has_sufficient_storage(out_dir)) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.compiling = false;
    state_.last_error = "Insufficient storage for MP4 compilation";
    return false;
  }

  const std::string ffmpeg = media_detail::resolve_executable("ffmpeg");
  if (ffmpeg.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.compiling = false;
    state_.last_error = "ffmpeg executable not found";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.compiling = true;
    state_.compile_current = 0;
    state_.compile_total = total_frames;
    state_.compile_progress = 0.0f;
    state_.last_error.clear();
  }
  {
    std::lock_guard<std::mutex> plock(compile_proc_mutex_);
    cancel_compile_requested_ = false;
  }

  std::string err;
  bool ok = run_compile_attempt(ffmpeg, paths, fps, resolution, true,
                                total_frames, on_progress, err);
  bool cancelled = false;
  {
    std::lock_guard<std::mutex> plock(compile_proc_mutex_);
    cancelled = cancel_compile_requested_;
  }

  if (!ok && !cancelled) {
    remove_silent(paths.mp4_tmp_path);
    ok = run_compile_attempt(ffmpeg, paths, fps, resolution, false,
                             total_frames, on_progress, err);
    std::lock_guard<std::mutex> plock(compile_proc_mutex_);
    cancelled = cancel_compile_requested_;
  }

  if (!ok || cancelled) {
    remove_silent(paths.mp4_tmp_path);
    std::lock_guard<std::mutex> lock(mutex_);
    state_.compiling = false;
    state_.last_error = cancelled ? "Compile cancelled" : err;
    return false;
  }

  std::string commit_err;
  if (!commit_staged_file(paths.mp4_tmp_path, paths.mp4_final_path,
                          commit_err)) {
    remove_silent(paths.mp4_tmp_path);
    std::lock_guard<std::mutex> lock(mutex_);
    state_.compiling = false;
    state_.last_error = commit_err;
    return false;
  }

  struct stat fin_st {};
  if (::stat(paths.mp4_final_path.c_str(), &fin_st) != 0 ||
      fin_st.st_size <= 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.compiling = false;
    state_.last_error = "final compiled MP4 missing or empty";
    return false;
  }

  std::error_code ec;
  fs::remove_all(paths.work_dir, ec);

  std::lock_guard<std::mutex> lock(mutex_);
  state_.compiling = false;
  state_.needs_compile = false;
  state_.compile_current = total_frames;
  state_.compile_total = total_frames;
  state_.compile_progress = 1.0f;
  state_.result_path = paths.mp4_final_path;
  state_.last_error.clear();
  return true;
}

void TimelapseEngine::cancel_compile() {
  std::lock_guard<std::mutex> plock(compile_proc_mutex_);
  cancel_compile_requested_ = true;
  if (cancel_wake_w_ >= 0) {
    const uint8_t b = 1;
    const ssize_t ignored = ::write(cancel_wake_w_, &b, 1);
    (void)ignored;
  }
  if (compile_pid_ > 0) {
    ::kill(compile_pid_, SIGTERM);
  }
}

bool TimelapseEngine::recover_session(const TimelapseManifest& manifest,
                                      RecoveryAction action,
                                      std::string& out_msg) {
  out_msg.clear();
  if (manifest.work_dir.empty()) {
    out_msg = "Invalid timelapse manifest work directory";
    return false;
  }

  std::string work_dir = manifest.work_dir;
  while (work_dir.size() > 1 && work_dir.back() == '/') {
    work_dir.pop_back();
  }
  const std::string out_dir = parent_dir_of(work_dir);

  std::string session_id = manifest.session_id;
  if (session_id.empty()) {
    const std::string base = fs::path(work_dir).filename().string();
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

  if (action == RecoveryAction::Discard) {
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    remove_silent(paths.mp4_tmp_path);
    if (ec) {
      out_msg = "Failed to discard work dir: " + ec.message();
      return false;
    }
    out_msg = "Discarded timelapse " + session_id;
    return true;
  }

  if (action == RecoveryAction::KeepPhotos) {
    if (!mkdir_p(paths.photos_dir)) {
      out_msg = "Failed to create destination folder";
      return false;
    }

    std::error_code ec;
    std::vector<fs::path> frames_to_move;
    for (const auto& entry : fs::directory_iterator(work_dir, ec)) {
      if (ec) {
        out_msg = "Failed scanning work dir: " + ec.message();
        return false;
      }
      const std::string fname = entry.path().filename().string();
      if (fname.empty() || fname[0] == '.') {
        continue;
      }
      if (!media_detail::ends_with_ci(fname, ".jpg") &&
          !media_detail::ends_with_ci(fname, ".jpeg")) {
        continue;
      }
      std::error_code file_ec;
      if (!entry.is_regular_file(file_ec) || file_ec) {
        continue;
      }
      const uintmax_t fsize = entry.file_size(file_ec);
      if (file_ec || fsize == 0) {
        continue;
      }
      frames_to_move.push_back(entry.path());
    }

    int moved = 0;
    for (const auto& src_path : frames_to_move) {
      const std::string fname = src_path.filename().string();
      const fs::path dst = fs::path(paths.photos_dir) / fname;
      fs::rename(src_path, dst, ec);
      if (ec) {
        out_msg = "Failed moving frame " + fname + ": " + ec.message();
        return false;
      }
      ++moved;
    }

    fs::remove_all(work_dir, ec);
    remove_silent(paths.mp4_tmp_path);

    if (moved <= 0) {
      fs::remove_all(paths.photos_dir, ec);
      out_msg = "No valid frames found in timelapse";
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      state_.result_path = paths.photos_dir;
    }
    out_msg = "Saved " + std::to_string(moved) + " photos to TL_" + session_id;
    return true;
  }

  // RecoveryAction::Compile
  const int fps = (manifest.fps > 0) ? manifest.fps : 24;
  const std::string res =
      manifest.resolution.empty() ? "2mp" : manifest.resolution;
  if (!compile_session(paths, fps, res)) {
    out_msg = state().last_error;
    return false;
  }
  out_msg = "Compiled TL_" + session_id + ".mp4";
  return true;
}

TimelapseState TimelapseEngine::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

TimelapseSessionPaths TimelapseEngine::session_paths() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_;
}

}  // namespace cz
