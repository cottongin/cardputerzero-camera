#include "filters.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>

namespace cz {

namespace {

constexpr int kFilterCount = 9;

inline uint8_t clamp_u8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return static_cast<uint8_t>(v);
}

inline int luma_bt601(int r, int g, int b) {
  return (77 * r + 150 * g + 29 * b) >> 8;
}

uint32_t hash_pixel_coords(int x, int y) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u +
               static_cast<uint32_t>(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

void apply_bw(uint8_t* rgb, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t* p = rgb + i * 3;
    const uint8_t y = static_cast<uint8_t>(luma_bt601(p[0], p[1], p[2]));
    p[0] = y;
    p[1] = y;
    p[2] = y;
  }
}

void apply_hicon(uint8_t* rgb, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t* p = rgb + i * 3;
    const int y = luma_bt601(p[0], p[1], p[2]);
    // ~1.6x contrast stretch around mid-gray (128)
    const uint8_t out = clamp_u8((((y - 128) * 410) >> 8) + 128);
    p[0] = out;
    p[1] = out;
    p[2] = out;
  }
}

void apply_sepia(uint8_t* rgb, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t* p = rgb + i * 3;
    const int r = p[0];
    const int g = p[1];
    const int b = p[2];
    p[0] = clamp_u8((101 * r + 197 * g + 48 * b) >> 8);
    p[1] = clamp_u8((89 * r + 176 * g + 43 * b) >> 8);
    p[2] = clamp_u8((70 * r + 137 * g + 34 * b) >> 8);
  }
}

void apply_cool(uint8_t* rgb, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t* p = rgb + i * 3;
    const int cr = (((static_cast<int>(p[0]) - 128) * 276) >> 8) + 128;
    const int cg = (((static_cast<int>(p[1]) - 128) * 276) >> 8) + 128;
    const int cb = (((static_cast<int>(p[2]) - 128) * 276) >> 8) + 128;
    p[0] = clamp_u8(cr - 14);
    p[1] = clamp_u8(cg + 2);
    p[2] = clamp_u8(cb + 20);
  }
}

void apply_vivid(uint8_t* rgb, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t* p = rgb + i * 3;
    const int r = p[0];
    const int g = p[1];
    const int b = p[2];
    const int y = luma_bt601(r, g, b);
    const int cy = (((y - 128) * 276) >> 8) + 128;
    p[0] = clamp_u8(cy + (((r - y) * 346) >> 8));
    p[1] = clamp_u8(cy + (((g - y) * 346) >> 8));
    p[2] = clamp_u8(cy + (((b - y) * 346) >> 8));
  }
}

void apply_invert(uint8_t* rgb, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t* p = rgb + i * 3;
    p[0] = static_cast<uint8_t>(255 - p[0]);
    p[1] = static_cast<uint8_t>(255 - p[1]);
    p[2] = static_cast<uint8_t>(255 - p[2]);
  }
}

void apply_retro(uint8_t* rgb, int width, int height) {
  const float inv_w = (width > 1) ? (2.0f / static_cast<float>(width - 1)) : 0.0f;
  const float inv_h = (height > 1) ? (2.0f / static_cast<float>(height - 1)) : 0.0f;

  for (int y = 0; y < height; ++y) {
    const float ny = (height > 1) ? (static_cast<float>(y) * inv_h - 1.0f) : 0.0f;
    const float ny2 = ny * ny;
    uint8_t* row = rgb + static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
    for (int x = 0; x < width; ++x) {
      const float nx = (width > 1) ? (static_cast<float>(x) * inv_w - 1.0f) : 0.0f;
      const float d2 = std::min(nx * nx + ny2, 2.0f);
      // Fixed-point vignette scale in [0, 256] (~0.72 at corners, 1.0 at center)
      const int vig_fp = static_cast<int>((1.0f - 0.14f * d2) * 256.0f);

      uint8_t* p = row + static_cast<size_t>(x) * 3;
      // Lifted shadows (+14) + highlight roll-off (*228/256) + warm midtone tint
      int r = ((static_cast<int>(p[0]) * 228) >> 8) + 24;
      int g = ((static_cast<int>(p[1]) * 228) >> 8) + 18;
      int b = ((static_cast<int>(p[2]) * 228) >> 8) + 8;

      r = (r * vig_fp) >> 8;
      g = (g * vig_fp) >> 8;
      b = (b * vig_fp) >> 8;

      // Deterministic subtle film grain in [-4, +3]
      const int grain = static_cast<int>(hash_pixel_coords(x, y) & 7u) - 4;
      p[0] = clamp_u8(r + grain);
      p[1] = clamp_u8(g + grain);
      p[2] = clamp_u8(b + grain);
    }
  }
}

void apply_gameboy(uint8_t* rgb, int width, int height) {
  static constexpr uint8_t kDmgPalette[4][3] = {
      {0x0f, 0x38, 0x0f},
      {0x30, 0x62, 0x30},
      {0x8b, 0xac, 0x0f},
      {0x9b, 0xbc, 0x0f},
  };
  static constexpr int kBayer4x4[4][4] = {
      {0, 8, 2, 10},
      {12, 4, 14, 6},
      {3, 11, 1, 9},
      {15, 7, 13, 5},
  };

  const int block = std::max(1, width / 160);

  for (int y0 = 0, by = 0; y0 < height; y0 += block, ++by) {
    const int y1 = std::min(height, y0 + block);
    for (int x0 = 0, bx = 0; x0 < width; x0 += block, ++bx) {
      const int x1 = std::min(width, x0 + block);

      int lum_sum = 0;
      int pix_count = 0;
      for (int y = y0; y < y1; ++y) {
        const uint8_t* row =
            rgb + static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
        for (int x = x0; x < x1; ++x) {
          const uint8_t* p = row + static_cast<size_t>(x) * 3;
          lum_sum += luma_bt601(p[0], p[1], p[2]);
          ++pix_count;
        }
      }

      const int avg_lum = (pix_count > 0) ? (lum_sum / pix_count) : 0;
      const int bayer = kBayer4x4[by & 3][bx & 3];
      const int shade =
          std::min(3, std::max(0, (avg_lum * 48 + bayer * 255) / 4080));
      const uint8_t* col = kDmgPalette[shade];

      for (int y = y0; y < y1; ++y) {
        uint8_t* row =
            rgb + static_cast<size_t>(y) * static_cast<size_t>(width) * 3;
        for (int x = x0; x < x1; ++x) {
          uint8_t* p = row + static_cast<size_t>(x) * 3;
          p[0] = col[0];
          p[1] = col[1];
          p[2] = col[2];
        }
      }
    }
  }
}

}  // namespace

std::string filter_to_string(FilterId id) {
  switch (id) {
    case FilterId::None:
      return "none";
    case FilterId::BW:
      return "bw";
    case FilterId::HiCon:
      return "hicon";
    case FilterId::Sepia:
      return "sepia";
    case FilterId::Cool:
      return "cool";
    case FilterId::Vivid:
      return "vivid";
    case FilterId::Invert:
      return "invert";
    case FilterId::Retro:
      return "retro";
    case FilterId::GameBoy:
      return "gameboy";
  }
  return "none";
}

FilterId filter_from_string(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "bw") return FilterId::BW;
  if (lower == "hicon") return FilterId::HiCon;
  if (lower == "sepia") return FilterId::Sepia;
  if (lower == "cool") return FilterId::Cool;
  if (lower == "vivid") return FilterId::Vivid;
  if (lower == "invert") return FilterId::Invert;
  if (lower == "retro") return FilterId::Retro;
  if (lower == "gameboy") return FilterId::GameBoy;
  return FilterId::None;
}

std::string filter_display_name(FilterId id) {
  switch (id) {
    case FilterId::None:
      return "None";
    case FilterId::BW:
      return "B&W";
    case FilterId::HiCon:
      return "Hi-Con";
    case FilterId::Sepia:
      return "Sepia";
    case FilterId::Cool:
      return "Cool";
    case FilterId::Vivid:
      return "Vivid";
    case FilterId::Invert:
      return "Invert";
    case FilterId::Retro:
      return "Retro";
    case FilterId::GameBoy:
      return "GameBoy";
  }
  return "None";
}

FilterId next_filter(FilterId current, int delta) {
  const int cur = static_cast<int>(current);
  int idx = (cur + delta) % kFilterCount;
  if (idx < 0) idx += kFilterCount;
  return static_cast<FilterId>(idx);
}

void apply_filter_rgb24(uint8_t* rgb, int width, int height, FilterId filter) {
  if (!rgb || width <= 0 || height <= 0 || filter == FilterId::None) {
    return;
  }
  const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
  switch (filter) {
    case FilterId::None:
      return;
    case FilterId::BW:
      apply_bw(rgb, count);
      return;
    case FilterId::HiCon:
      apply_hicon(rgb, count);
      return;
    case FilterId::Sepia:
      apply_sepia(rgb, count);
      return;
    case FilterId::Cool:
      apply_cool(rgb, count);
      return;
    case FilterId::Vivid:
      apply_vivid(rgb, count);
      return;
    case FilterId::Invert:
      apply_invert(rgb, count);
      return;
    case FilterId::Retro:
      apply_retro(rgb, width, height);
      return;
    case FilterId::GameBoy:
      apply_gameboy(rgb, width, height);
      return;
  }
}

void scale_crop_rgb24(const uint8_t* src,
                      int src_w,
                      int src_h,
                      uint8_t* dst,
                      int dst_w,
                      int dst_h,
                      float loupe_factor) {
  if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
    return;
  }
  const double loupe = (loupe_factor < 1.0f) ? 1.0 : static_cast<double>(loupe_factor);

  const double src_aspect = static_cast<double>(src_w) / static_cast<double>(src_h);
  const double dst_aspect = static_cast<double>(dst_w) / static_cast<double>(dst_h);

  double crop_w = static_cast<double>(src_w);
  double crop_h = static_cast<double>(src_h);
  if (src_aspect > dst_aspect) {
    crop_w = static_cast<double>(src_h) * dst_aspect;
  } else if (src_aspect < dst_aspect) {
    crop_h = static_cast<double>(src_w) / dst_aspect;
  }

  crop_w /= loupe;
  crop_h /= loupe;
  const double x0 = (static_cast<double>(src_w) - crop_w) * 0.5;
  const double y0 = (static_cast<double>(src_h) - crop_h) * 0.5;

  const double step_x = crop_w / static_cast<double>(dst_w);
  const double step_y = crop_h / static_cast<double>(dst_h);

  for (int dy = 0; dy < dst_h; ++dy) {
    double sy = y0 + (static_cast<double>(dy) + 0.5) * step_y - 0.5;
    if (sy < 0.0) sy = 0.0;
    if (sy > static_cast<double>(src_h - 1)) sy = static_cast<double>(src_h - 1);

    const int iy0 = static_cast<int>(std::floor(sy));
    const int iy1 = std::min(src_h - 1, iy0 + 1);
    const int fy = static_cast<int>((sy - static_cast<double>(iy0)) * 256.0);
    const int ify = 256 - fy;

    const uint8_t* row0 = src + static_cast<size_t>(iy0) * static_cast<size_t>(src_w) * 3;
    const uint8_t* row1 = src + static_cast<size_t>(iy1) * static_cast<size_t>(src_w) * 3;
    uint8_t* dst_row = dst + static_cast<size_t>(dy) * static_cast<size_t>(dst_w) * 3;

    for (int dx = 0; dx < dst_w; ++dx) {
      double sx = x0 + (static_cast<double>(dx) + 0.5) * step_x - 0.5;
      if (sx < 0.0) sx = 0.0;
      if (sx > static_cast<double>(src_w - 1)) sx = static_cast<double>(src_w - 1);

      const int ix0 = static_cast<int>(std::floor(sx));
      const int ix1 = std::min(src_w - 1, ix0 + 1);
      const int fx = static_cast<int>((sx - static_cast<double>(ix0)) * 256.0);
      const int ifx = 256 - fx;

      const int w00 = ifx * ify;
      const int w10 = fx * ify;
      const int w01 = ifx * fy;
      const int w11 = fx * fy;

      const uint8_t* p00 = row0 + static_cast<size_t>(ix0) * 3;
      const uint8_t* p10 = row0 + static_cast<size_t>(ix1) * 3;
      const uint8_t* p01 = row1 + static_cast<size_t>(ix0) * 3;
      const uint8_t* p11 = row1 + static_cast<size_t>(ix1) * 3;

      uint8_t* out = dst_row + static_cast<size_t>(dx) * 3;
      for (int c = 0; c < 3; ++c) {
        out[c] = static_cast<uint8_t>(
            (p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11) >> 16);
      }
    }
  }
}

}  // namespace cz
