#include "storage.hpp"
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace cz {

namespace {

std::string parent_dir_of(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return ".";
  }
  if (slash == 0) {
    return "/";
  }
  return path.substr(0, slash);
}

std::string base_name_of(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

std::string join_path(const std::string& dir, const std::string& name) {
  if (dir.empty()) {
    return name;
  }
  if (dir.back() == '/') {
    return dir + name;
  }
  return dir + "/" + name;
}

bool path_exists_lstat(const std::string& path) {
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0;
}

std::string current_timestamp_compact() {
  const std::time_t now = std::time(nullptr);
  struct tm tm_now {};
  ::localtime_r(&now, &tm_now);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
  return std::string(buf);
}

void fsync_parent_best_effort(const std::string& path) {
  const std::string dir = parent_dir_of(path);
  const int dfd = ::open(dir.c_str(), O_RDONLY);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
}

std::string resolve_manifest_path(const std::string& path_or_dir) {
  const std::string expanded = expand_user_path(path_or_dir);
  if (expanded.size() >= 5 &&
      expanded.compare(expanded.size() - 5, 5, ".json") == 0) {
    return expanded;
  }
  struct stat st {};
  if (::stat(expanded.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
    return expanded;
  }
  return join_path(expanded, "session.json");
}

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

int count_valid_frames_in_dir(const std::string& dir_path) {
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
    if (!ends_with_ci(name, ".jpg") && !ends_with_ci(name, ".jpeg")) {
      continue;
    }
    const std::string full = join_path(dir_path, name);
    struct stat st {};
    if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > 0) {
      ++count;
    }
  }
  ::closedir(d);
  return count;
}

StagedPaths next_media_paths(const std::string& output_dir,
                             const std::string& prefix,
                             const std::string& final_ext,
                             const std::string& tmp_ext) {
  const std::string dir = expand_user_path(output_dir);
  mkdir_p(dir);
  const std::string ts = current_timestamp_compact();

  for (int idx = 0; idx < 1000; ++idx) {
    std::string suffix;
    if (idx > 0) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "_%02d", idx);
      suffix = buf;
    }
    const std::string stem = prefix + "_" + ts + suffix;
    const std::string tmp_p = join_path(dir, "." + stem + tmp_ext);
    const std::string fin_p = join_path(dir, stem + final_ext);
    if (!path_exists_lstat(tmp_p) && !path_exists_lstat(fin_p)) {
      return StagedPaths{tmp_p, fin_p};
    }
  }

  const std::string fallback_stem =
      prefix + "_" + ts + "_" + std::to_string(::getpid());
  return StagedPaths{join_path(dir, "." + fallback_stem + tmp_ext),
                     join_path(dir, fallback_stem + final_ext)};
}

}  // namespace

bool mkdir_p(const std::string& path) {
  const std::string expanded = expand_user_path(path);
  if (expanded.empty()) {
    return false;
  }

  std::string current;
  size_t i = 0;
  if (expanded[0] == '/') {
    current = "/";
    i = 1;
  }

  while (i <= expanded.size()) {
    const size_t next = expanded.find('/', i);
    const std::string part = (next == std::string::npos)
                                 ? expanded.substr(i)
                                 : expanded.substr(i, next - i);
    if (!part.empty()) {
      current = join_path(current, part);
      struct stat st {};
      if (::stat(current.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
          return false;
        }
      } else if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
      }
    }
    if (next == std::string::npos) {
      break;
    }
    i = next + 1;
  }

  struct stat st {};
  return ::stat(expanded.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool write_file_atomic(const std::string& tmp_path,
                       const std::string& final_path,
                       const std::string& content,
                       std::string& out_err) {
  const std::string parent = parent_dir_of(final_path);
  if (!mkdir_p(parent)) {
    out_err = "Failed to create directory: " + parent;
    return false;
  }

  const int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    out_err = std::string("Failed to open temp file: ") + std::strerror(errno);
    return false;
  }

  size_t written = 0;
  while (written < content.size()) {
    const ssize_t n =
        ::write(fd, content.data() + written, content.size() - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      out_err = std::string("Failed writing temp file: ") + std::strerror(errno);
      ::close(fd);
      ::unlink(tmp_path.c_str());
      return false;
    }
    written += static_cast<size_t>(n);
  }

  if (::fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
    out_err = std::string("fsync failed: ") + std::strerror(errno);
    ::close(fd);
    ::unlink(tmp_path.c_str());
    return false;
  }
  if (::close(fd) != 0) {
    out_err = std::string("close failed: ") + std::strerror(errno);
    ::unlink(tmp_path.c_str());
    return false;
  }

  if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    out_err = std::string("rename failed: ") + std::strerror(errno);
    ::unlink(tmp_path.c_str());
    return false;
  }

  fsync_parent_best_effort(final_path);
  out_err.clear();
  return true;
}

StagedPaths next_photo_paths(const std::string& output_dir) {
  return next_media_paths(output_dir, "IMG", ".jpg", ".jpg.tmp");
}

StagedPaths next_video_paths(const std::string& output_dir) {
  return next_media_paths(output_dir, "VID", ".mp4", ".mp4.part");
}

TimelapseSessionPaths create_timelapse_session(const std::string& output_dir,
                                               bool create_work_dir) {
  const std::string dir = expand_user_path(output_dir);
  mkdir_p(dir);
  const std::string ts = current_timestamp_compact();

  std::string session_id = ts;
  for (int idx = 0; idx < 1000; ++idx) {
    std::string candidate = ts;
    if (idx > 0) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "_%02d", idx);
      candidate += buf;
    }
    const std::string work = join_path(dir, ".tl_work_" + candidate);
    const std::string photos = join_path(dir, "TL_" + candidate);
    const std::string mp4_tmp = join_path(dir, ".TL_" + candidate + ".mp4.part");
    const std::string mp4_fin = join_path(dir, "TL_" + candidate + ".mp4");
    if (!path_exists_lstat(work) && !path_exists_lstat(photos) &&
        !path_exists_lstat(mp4_tmp) && !path_exists_lstat(mp4_fin)) {
      session_id = candidate;
      break;
    }
  }

  TimelapseSessionPaths paths;
  paths.session_id = session_id;
  paths.work_dir = join_path(dir, ".tl_work_" + session_id);
  paths.photos_dir = join_path(dir, "TL_" + session_id);
  paths.manifest_path = join_path(paths.work_dir, "session.json");
  paths.mp4_tmp_path = join_path(dir, ".TL_" + session_id + ".mp4.part");
  paths.mp4_final_path = join_path(dir, "TL_" + session_id + ".mp4");

  if (create_work_dir) {
    mkdir_p(paths.work_dir);
  }
  return paths;
}

StagedPaths next_timelapse_frame_paths(const std::string& dir, int frame_index) {
  const std::string expanded = expand_user_path(dir);
  char name_buf[32];
  std::snprintf(name_buf, sizeof(name_buf), "frame_%04d.jpg",
                std::max(1, frame_index));
  const std::string frame_name(name_buf);
  return StagedPaths{join_path(expanded, "." + frame_name + ".tmp"),
                     join_path(expanded, frame_name)};
}

bool commit_staged_file(const std::string& tmp_path,
                        const std::string& final_path,
                        std::string& out_err) {
  struct stat st {};
  if (::stat(tmp_path.c_str(), &st) != 0) {
    out_err = std::string("Staged file missing: ") + std::strerror(errno);
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    out_err = "Staged path is not a regular file";
    return false;
  }
  if (st.st_size <= 0) {
    out_err = "Staged file is empty (0 bytes)";
    return false;
  }

  const int fd = ::open(tmp_path.c_str(), O_RDONLY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }

  if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
    out_err = std::string("Atomic rename failed: ") + std::strerror(errno);
    return false;
  }

  fsync_parent_best_effort(final_path);
  out_err.clear();
  return true;
}

bool has_sufficient_storage(const std::string& dir_path,
                            uint64_t min_bytes,
                            uint64_t* out_free_bytes) {
  std::string probe = expand_user_path(dir_path);
  if (probe.empty()) {
    probe = ".";
  }

  struct statvfs vfs {};
  while (::statvfs(probe.c_str(), &vfs) != 0) {
    if (errno != ENOENT || probe == "/" || probe == ".") {
      if (out_free_bytes) {
        *out_free_bytes = 0;
      }
      return false;
    }
    const std::string next = parent_dir_of(probe);
    if (next == probe) {
      if (out_free_bytes) {
        *out_free_bytes = 0;
      }
      return false;
    }
    probe = next;
  }

  const uint64_t block_size =
      vfs.f_frsize > 0 ? static_cast<uint64_t>(vfs.f_frsize)
                       : static_cast<uint64_t>(vfs.f_bsize);
  const uint64_t avail = static_cast<uint64_t>(vfs.f_bavail);
  const uint64_t free_bytes =
      (block_size > 0 && avail > UINT64_MAX / block_size)
          ? UINT64_MAX
          : avail * block_size;
  if (out_free_bytes) {
    *out_free_bytes = free_bytes;
  }
  return free_bytes >= min_bytes;
}

bool write_timelapse_manifest(const std::string& path_or_dir,
                              const TimelapseManifest& manifest) {
  const std::string manifest_path = resolve_manifest_path(path_or_dir);
  const std::string dir = parent_dir_of(manifest_path);
  const std::string base = base_name_of(manifest_path);
  const std::string tmp_path =
      join_path(dir, "." + base + "." + std::to_string(::getpid()) + ".tmp");

  std::ostringstream oss;
  oss << "{\n"
      << "  \"session_id\": \""
      << json_detail::escape_json_string(manifest.session_id) << "\",\n"
      << "  \"created_at\": \""
      << json_detail::escape_json_string(manifest.created_at) << "\",\n"
      << "  \"resolution\": \""
      << json_detail::escape_json_string(manifest.resolution) << "\",\n"
      << "  \"interval_sec\": " << manifest.interval_sec << ",\n"
      << "  \"fps\": " << manifest.fps << ",\n"
      << "  \"frame_count\": " << manifest.frame_count << ",\n"
      << "  \"status\": \"" << json_detail::escape_json_string(manifest.status)
      << "\"\n"
      << "}\n";

  std::string err;
  return write_file_atomic(tmp_path, manifest_path, oss.str(), err);
}

bool read_timelapse_manifest(const std::string& path_or_dir,
                             TimelapseManifest& out_manifest) {
  const std::string manifest_path = resolve_manifest_path(path_or_dir);
  std::ifstream in(manifest_path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  const std::string contents((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  std::map<std::string, json_detail::JsonValue> obj;
  if (!json_detail::parse_json_object(contents, obj)) {
    return false;
  }

  TimelapseManifest m;
  m.work_dir = parent_dir_of(manifest_path);
  const std::string dir_name = base_name_of(m.work_dir);
  if (dir_name.rfind(".tl_work_", 0) == 0) {
    m.session_id = dir_name.substr(9);
  }

  auto get_str = [&](const char* key, std::string& dst) {
    auto it = obj.find(key);
    if (it != obj.end() &&
        it->second.type == json_detail::JsonValue::Type::String) {
      dst = it->second.str_val;
    }
  };
  auto get_int = [&](const char* key, int& dst) {
    auto it = obj.find(key);
    if (it != obj.end() &&
        it->second.type == json_detail::JsonValue::Type::Number) {
      dst = static_cast<int>(std::llround(it->second.num_val));
    }
  };

  get_str("session_id", m.session_id);
  get_str("created_at", m.created_at);
  get_str("resolution", m.resolution);
  get_int("interval_sec", m.interval_sec);
  get_int("fps", m.fps);
  get_int("frame_count", m.frame_count);
  get_str("status", m.status);

  out_manifest = m;
  return true;
}

std::vector<TimelapseManifest> scan_orphaned_timelapses(
    const std::string& output_dir) {
  std::vector<TimelapseManifest> results;
  const std::string dir = expand_user_path(output_dir);
  DIR* d = ::opendir(dir.c_str());
  if (!d) {
    return results;
  }

  struct dirent* ent = nullptr;
  while ((ent = ::readdir(d)) != nullptr) {
    const std::string name(ent->d_name);
    if (name.rfind(".tl_work_", 0) != 0) {
      continue;
    }
    const std::string work_dir = join_path(dir, name);
    struct stat st {};
    if (::stat(work_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
      continue;
    }
    const int valid_frames = count_valid_frames_in_dir(work_dir);
    if (valid_frames <= 0) {
      continue;
    }

    TimelapseManifest m;
    if (!read_timelapse_manifest(work_dir, m)) {
      m.session_id = name.substr(9);
      m.work_dir = work_dir;
      m.status = "orphaned";
    }
    if (m.session_id.empty()) {
      m.session_id = name.substr(9);
    }
    m.work_dir = work_dir;
    m.frame_count = valid_frames;
    results.push_back(m);
  }
  ::closedir(d);

  std::sort(results.begin(), results.end(),
            [](const TimelapseManifest& a, const TimelapseManifest& b) {
              return a.session_id < b.session_id;
            });
  return results;
}

}  // namespace cz
