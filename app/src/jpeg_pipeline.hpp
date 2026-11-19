#pragma once

#include "filters.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cz {

struct RgbImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgb24;
};

bool decode_jpeg_rgb24(const std::string& path,
                       RgbImage& out,
                       std::string& out_err);

bool decode_jpeg_scaled_rgb24(const std::string& path,
                              int target_w,
                              int target_h,
                              RgbImage& out,
                              std::string& out_err);

bool encode_jpeg_rgb24(const std::string& path,
                       const uint8_t* rgb,
                       int width,
                       int height,
                       int quality,
                       std::string& out_err);

bool finalize_still_photo(const std::string& tmp_path,
                          const std::string& final_path,
                          FilterId filter,
                          std::string& out_err);

}  // namespace cz
