#include "backlight.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace cz {

namespace {

std::string join_path(const std::string& dir, const std::string& name) {
  if (dir.empty()) {
    return name;
  }
  if (dir.back() == '/') {
    return dir + name;
  }
  return dir + "/" + name;
}

std::string trim_ascii_ws(const std::string& s) {
  size_t start = 0;
  while (start < s.size() &&
         std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }
  size_t end = s.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(start, end - start);
}

}  // namespace

BacklightController::BacklightController(const std::string& sysfs_root)
    : sysfs_root_(sysfs_root) {
  std::lock_guard<std::mutex> lock(mutex_);
  probe_sysfs_locked();
}

BacklightController::~BacklightController() { restore(); }

void BacklightController::probe_sysfs_locked() {
  has_hardware_backlight_ = false;
  brightness_path_.clear();
  max_brightness_path_.clear();

  if (sysfs_root_.empty()) {
    return;
  }

  DIR* dir = ::opendir(sysfs_root_.c_str());
  if (!dir) {
    return;
  }

  std::vector<std::string> entries;
  struct dirent* ent = nullptr;
  while ((ent = ::readdir(dir)) != nullptr) {
    const std::string name(ent->d_name);
    if (name.empty() || name[0] == '.') {
      continue;
    }
    entries.push_back(name);
  }
  ::closedir(dir);

  std::sort(entries.begin(), entries.end());

  for (const auto& entry : entries) {
    const std::string dev_dir = join_path(sysfs_root_, entry);
    struct stat dir_st {};
    if (::stat(dev_dir.c_str(), &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) {
      continue;
    }
    const std::string b_path = join_path(dev_dir, "brightness");
    struct stat b_st {};
    if (::stat(b_path.c_str(), &b_st) != 0 || !S_ISREG(b_st.st_mode)) {
      continue;
    }
    if ((b_st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) {
      continue;
    }
    const int fd = ::open(b_path.c_str(), O_RDWR);
    if (fd >= 0) {
      ::close(fd);
      brightness_path_ = b_path;
      max_brightness_path_ = join_path(dev_dir, "max_brightness");
      has_hardware_backlight_ = true;
      return;
    }
  }
}

bool BacklightController::read_trimmed_file_locked(const std::string& path,
                                                   std::string& out_value) const {
  out_value.clear();
  if (path.empty()) {
    return false;
  }
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  char buf[64];
  std::string raw;
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      raw.append(buf, static_cast<size_t>(n));
      if (raw.size() >= 256) {
        break;
      }
      continue;
    }
    if (n == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    ::close(fd);
    return false;
  }
  ::close(fd);
  out_value = trim_ascii_ws(raw);
  return !out_value.empty();
}

bool BacklightController::write_file_locked(const std::string& path,
                                            const std::string& value) const {
  if (path.empty()) {
    return false;
  }
  const int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
  if (fd < 0) {
    return false;
  }
  size_t off = 0;
  while (off < value.size()) {
    const ssize_t w = ::write(fd, value.data() + off, value.size() - off);
    if (w > 0) {
      off += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR) {
      continue;
    }
    ::close(fd);
    return false;
  }
  ::close(fd);
  return true;
}

bool BacklightController::dim() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (dimmed_) {
    return true;
  }

  bool ok = true;
  if (has_hardware_backlight_) {
    std::string cur;
    if (!read_trimmed_file_locked(brightness_path_, cur)) {
      if (!read_trimmed_file_locked(max_brightness_path_, cur)) {
        cur = "128";
      }
    }
    saved_brightness_ = cur;
    ok = write_file_locked(brightness_path_, "0");
  }

  dimmed_ = true;
  return ok;
}

bool BacklightController::restore() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!dimmed_) {
    return true;
  }

  bool ok = true;
  if (has_hardware_backlight_ && !saved_brightness_.empty()) {
    ok = write_file_locked(brightness_path_, saved_brightness_);
  }

  dimmed_ = false;
  saved_brightness_.clear();
  return ok;
}

bool BacklightController::is_dimmed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dimmed_;
}

bool BacklightController::has_hardware_backlight() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_hardware_backlight_;
}

}  // namespace cz
