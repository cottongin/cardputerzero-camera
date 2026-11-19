#pragma once

#include <mutex>
#include <string>

namespace cz {

class BacklightController {
 public:
  explicit BacklightController(
      const std::string& sysfs_root = "/sys/class/backlight");
  ~BacklightController();

  BacklightController(const BacklightController&) = delete;
  BacklightController& operator=(const BacklightController&) = delete;

  bool dim();
  bool restore();
  bool is_dimmed() const;
  bool has_hardware_backlight() const;

 private:
  void probe_sysfs_locked();
  bool read_trimmed_file_locked(const std::string& path,
                                std::string& out_value) const;
  bool write_file_locked(const std::string& path,
                         const std::string& value) const;

  mutable std::mutex mutex_;
  std::string sysfs_root_;
  std::string brightness_path_;
  std::string max_brightness_path_;
  std::string saved_brightness_;
  bool has_hardware_backlight_ = false;
  bool dimmed_ = false;
};

}  // namespace cz
