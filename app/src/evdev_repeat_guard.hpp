#pragma once

#include <cstdint>
#include <unordered_set>

namespace cz {

/// Suppresses Linux evdev auto-repeat events (value == 2) for any key whose
/// initial press (value == 1) arrived while the display was dimmed.
class EvdevRepeatGuard {
 public:
  bool should_dispatch(uint16_t code, int32_t value, bool is_dimmed) {
    if (value == 0) {
      suppressed_codes_.erase(code);
      return false;
    }
    if (value == 1) {
      if (is_dimmed) {
        suppressed_codes_.insert(code);
      } else {
        suppressed_codes_.erase(code);
      }
      return true;
    }
    if (value == 2) {
      if (is_dimmed) {
        suppressed_codes_.insert(code);
        return true;
      }
      return suppressed_codes_.count(code) == 0;
    }
    return false;
  }

 private:
  std::unordered_set<uint16_t> suppressed_codes_;
};

}  // namespace cz
