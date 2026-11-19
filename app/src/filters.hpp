#pragma once

#include <cstdint>
#include <string>

namespace cz {

enum class FilterId {
  None,
  BW,
  HiCon,
  Sepia,
  Cool,
  Vivid,
  Invert,
  Retro,
  GameBoy,
};

std::string filter_to_string(FilterId id);
FilterId filter_from_string(const std::string& value);
std::string filter_display_name(FilterId id);
FilterId next_filter(FilterId current, int delta = 1);

void apply_filter_rgb24(uint8_t* rgb, int width, int height, FilterId filter);

void scale_crop_rgb24(const uint8_t* src,
                      int src_w,
                      int src_h,
                      uint8_t* dst,
                      int dst_w,
                      int dst_h,
                      float loupe_factor = 1.0f);

}  // namespace cz
