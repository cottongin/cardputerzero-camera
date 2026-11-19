#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cz {

struct FbChannel {
  uint32_t offset = 0;
  uint32_t length = 0;
};

struct FbBlitSpec {
  uint32_t xres = 0;
  uint32_t yres = 0;
  uint32_t xres_virtual = 0;
  uint32_t yres_virtual = 0;
  uint32_t xoffset = 0;
  uint32_t yoffset = 0;
  uint32_t bits_per_pixel = 0;
  uint32_t line_length = 0;
  FbChannel red;
  FbChannel green;
  FbChannel blue;
  FbChannel transp;
};

inline uint32_t pack_fb_channel(uint32_t value_8bit, const FbChannel& ch) {
  if (ch.length == 0 || ch.offset >= 32) {
    return 0u;
  }
  const uint32_t v = value_8bit & 0xFFu;
  uint32_t scaled = 0u;
  if (ch.length >= 8u) {
    const uint32_t shift = std::min(ch.length - 8u, 24u);
    scaled = v << shift;
  } else {
    scaled = v >> (8u - ch.length);
  }
  if (ch.length < 32u) {
    scaled &= (1u << ch.length) - 1u;
  }
  return scaled << ch.offset;
}

inline bool valid_fb_channel(const FbChannel& ch, uint32_t bpp, bool required) {
  if (ch.length == 0u) {
    return !required;
  }
  if (ch.offset >= bpp || ch.length > bpp) {
    return false;
  }
  return static_cast<uint64_t>(ch.offset) + ch.length <= bpp;
}

inline bool blit_framebuffer(uint8_t* fb_map,
                             size_t map_size,
                             const FbBlitSpec& spec,
                             const uint32_t* src_rgb888,
                             int src_w,
                             int src_h) {
  if (!fb_map || !src_rgb888 || src_w <= 0 || src_h <= 0) {
    return false;
  }
  if (spec.xres == 0 || spec.yres == 0) {
    return false;
  }
  if (spec.bits_per_pixel != 16u && spec.bits_per_pixel != 24u &&
      spec.bits_per_pixel != 32u) {
    return false;
  }
  if (!valid_fb_channel(spec.red, spec.bits_per_pixel, true) ||
      !valid_fb_channel(spec.green, spec.bits_per_pixel, true) ||
      !valid_fb_channel(spec.blue, spec.bits_per_pixel, true) ||
      !valid_fb_channel(spec.transp, spec.bits_per_pixel, false)) {
    return false;
  }

  const uint64_t eff_xvirt =
      spec.xres_virtual ? spec.xres_virtual : spec.xres;
  const uint64_t eff_yvirt =
      spec.yres_virtual ? spec.yres_virtual : spec.yres;
  if (static_cast<uint64_t>(spec.xoffset) + spec.xres > eff_xvirt ||
      static_cast<uint64_t>(spec.yoffset) + spec.yres > eff_yvirt) {
    return false;
  }

  const uint64_t copy_w =
      std::min<uint64_t>(spec.xres, static_cast<uint64_t>(src_w));
  const uint64_t copy_h =
      std::min<uint64_t>(spec.yres, static_cast<uint64_t>(src_h));
  const uint64_t bytes_pp = spec.bits_per_pixel / 8u;

  const uint64_t row_bytes_needed =
      (static_cast<uint64_t>(spec.xoffset) + copy_w) * bytes_pp;
  if (spec.line_length == 0 || spec.line_length < row_bytes_needed) {
    return false;
  }

  const uint64_t last_row = static_cast<uint64_t>(spec.yoffset) + copy_h - 1u;
  if (last_row > 0 &&
      spec.line_length > (static_cast<uint64_t>(SIZE_MAX) - row_bytes_needed) / last_row) {
    return false;
  }
  const uint64_t required_bytes =
      last_row * static_cast<uint64_t>(spec.line_length) + row_bytes_needed;
  if (required_bytes > static_cast<uint64_t>(map_size)) {
    return false;
  }

  for (uint64_t y = 0; y < copy_h; ++y) {
    const uint64_t dst_row_off =
        (static_cast<uint64_t>(spec.yoffset) + y) * spec.line_length +
        static_cast<uint64_t>(spec.xoffset) * bytes_pp;
    uint8_t* dst = fb_map + dst_row_off;
    const uint32_t* src_row = src_rgb888 + y * static_cast<uint64_t>(src_w);

    for (uint64_t x = 0; x < copy_w; ++x) {
      const uint32_t rgb = src_row[x];
      const uint32_t r = (rgb >> 16) & 0xFFu;
      const uint32_t g = (rgb >> 8) & 0xFFu;
      const uint32_t b = rgb & 0xFFu;
      const uint32_t packed = pack_fb_channel(r, spec.red) |
                              pack_fb_channel(g, spec.green) |
                              pack_fb_channel(b, spec.blue) |
                              pack_fb_channel(0xFFu, spec.transp);
      uint8_t* px = dst + x * bytes_pp;
      px[0] = static_cast<uint8_t>(packed & 0xFFu);
      px[1] = static_cast<uint8_t>((packed >> 8) & 0xFFu);
      if (bytes_pp >= 3u) {
        px[2] = static_cast<uint8_t>((packed >> 16) & 0xFFu);
      }
      if (bytes_pp == 4u) {
        px[3] = static_cast<uint8_t>((packed >> 24) & 0xFFu);
      }
    }
  }

  return true;
}

}  // namespace cz
