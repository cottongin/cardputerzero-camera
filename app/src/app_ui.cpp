#include "app_ui.hpp"

#include "filters.hpp"
#include "video_player.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(CP0_HAS_LVGL)
#include <lvgl.h>
#endif

#if defined(CZ_HAS_FREETYPE)
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace cz {

namespace {

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

std::string format_bytes_short(uint64_t bytes) {
  std::ostringstream oss;
  if (bytes < 1024ULL) {
    oss << bytes << "B";
  } else if (bytes < 1024ULL * 1024ULL) {
    oss << std::fixed << std::setprecision(0)
        << (static_cast<double>(bytes) / 1024.0) << "K";
  } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
    oss << std::fixed << std::setprecision(1)
        << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << "M";
  } else {
    oss << std::fixed << std::setprecision(1)
        << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << "G";
  }
  return oss.str();
}

std::string format_mmss(int total_sec) {
  if (total_sec < 0) total_sec = 0;
  const int mins = total_sec / 60;
  const int secs = total_sec % 60;
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
  return std::string(buf);
}

std::string format_interval_short(int sec) {
  if (sec >= 60 && (sec % 60) == 0) {
    return std::to_string(sec / 60) + "m";
  }
  return std::to_string(sec) + "s";
}

std::string format_zoom_str(float z) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.1fx", static_cast<double>(z));
  return std::string(buf);
}

std::string format_ev_str(double ev) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%+.1f", ev);
  return std::string(buf);
}

std::string format_res_upper(const std::string& res) {
  if (res == "8mp") return "8MP";
  if (res == "2mp") return "2MP";
  return res;
}

std::string format_orient_display(const std::string& t) {
  if (t == "0") return "0 deg";
  if (t == "180") return "180 deg";
  if (t == "hflip") return "H-Flip";
  if (t == "vflip") return "V-Flip";
  return "Auto";
}

std::string format_stop_short(const std::string& mode, int val) {
  if (mode == "shots") return std::to_string(val) + "#";
  if (mode == "duration") {
    if (val >= 3600 && (val % 3600) == 0) return std::to_string(val / 3600) + "h";
    if (val >= 60) return std::to_string(val / 60) + "m";
    return std::to_string(val) + "s";
  }
  return "Man";
}

std::string pad3(int n) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%03d", std::max(0, n));
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// Built-in 5x7 ASCII bitmap font (0x20..0x7E) fallback
// ---------------------------------------------------------------------------

constexpr std::array<std::array<uint8_t, 5>, 95> kFont5x7 = {{
    {0x00, 0x00, 0x00, 0x00, 0x00},  // ' '
    {0x00, 0x00, 0x5f, 0x00, 0x00},  // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00},  // '"'
    {0x14, 0x7f, 0x14, 0x7f, 0x14},  // '#'
    {0x24, 0x2a, 0x7f, 0x2a, 0x12},  // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62},  // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50},  // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00},  // '\''
    {0x00, 0x1c, 0x22, 0x41, 0x00},  // '('
    {0x00, 0x41, 0x22, 0x1c, 0x00},  // ')'
    {0x14, 0x08, 0x3e, 0x08, 0x14},  // '*'
    {0x08, 0x08, 0x3e, 0x08, 0x08},  // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00},  // ','
    {0x08, 0x08, 0x08, 0x08, 0x08},  // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00},  // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02},  // '/'
    {0x3e, 0x51, 0x49, 0x45, 0x3e},  // '0'
    {0x00, 0x42, 0x7f, 0x40, 0x00},  // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46},  // '2'
    {0x21, 0x41, 0x45, 0x4b, 0x31},  // '3'
    {0x18, 0x14, 0x12, 0x7f, 0x10},  // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39},  // '5'
    {0x3c, 0x4a, 0x49, 0x49, 0x30},  // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03},  // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36},  // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1e},  // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00},  // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00},  // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00},  // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14},  // '='
    {0x00, 0x41, 0x22, 0x14, 0x08},  // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06},  // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3e},  // '@'
    {0x7e, 0x11, 0x11, 0x11, 0x7e},  // 'A'
    {0x7f, 0x49, 0x49, 0x49, 0x36},  // 'B'
    {0x3e, 0x41, 0x41, 0x41, 0x22},  // 'C'
    {0x7f, 0x41, 0x41, 0x22, 0x1c},  // 'D'
    {0x7f, 0x49, 0x49, 0x49, 0x41},  // 'E'
    {0x7f, 0x09, 0x09, 0x09, 0x01},  // 'F'
    {0x3e, 0x41, 0x49, 0x49, 0x7a},  // 'G'
    {0x7f, 0x08, 0x08, 0x08, 0x7f},  // 'H'
    {0x00, 0x41, 0x7f, 0x41, 0x00},  // 'I'
    {0x20, 0x40, 0x41, 0x3f, 0x01},  // 'J'
    {0x7f, 0x08, 0x14, 0x22, 0x41},  // 'K'
    {0x7f, 0x40, 0x40, 0x40, 0x40},  // 'L'
    {0x7f, 0x02, 0x0c, 0x02, 0x7f},  // 'M'
    {0x7f, 0x04, 0x08, 0x10, 0x7f},  // 'N'
    {0x3e, 0x41, 0x41, 0x41, 0x3e},  // 'O'
    {0x7f, 0x09, 0x09, 0x09, 0x06},  // 'P'
    {0x3e, 0x41, 0x51, 0x21, 0x5e},  // 'Q'
    {0x7f, 0x09, 0x19, 0x29, 0x46},  // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31},  // 'S'
    {0x01, 0x01, 0x7f, 0x01, 0x01},  // 'T'
    {0x3f, 0x40, 0x40, 0x40, 0x3f},  // 'U'
    {0x1f, 0x20, 0x40, 0x20, 0x1f},  // 'V'
    {0x3f, 0x40, 0x38, 0x40, 0x3f},  // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63},  // 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07},  // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43},  // 'Z'
    {0x00, 0x7f, 0x41, 0x41, 0x00},  // '['
    {0x02, 0x04, 0x08, 0x10, 0x20},  // '\'
    {0x00, 0x41, 0x41, 0x7f, 0x00},  // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04},  // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40},  // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00},  // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78},  // 'a'
    {0x7f, 0x48, 0x44, 0x44, 0x38},  // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20},  // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7f},  // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18},  // 'e'
    {0x08, 0x7e, 0x09, 0x01, 0x02},  // 'f'
    {0x0c, 0x52, 0x52, 0x52, 0x3e},  // 'g'
    {0x7f, 0x08, 0x04, 0x04, 0x78},  // 'h'
    {0x00, 0x44, 0x7d, 0x40, 0x00},  // 'i'
    {0x20, 0x40, 0x44, 0x3d, 0x00},  // 'j'
    {0x7f, 0x10, 0x28, 0x44, 0x00},  // 'k'
    {0x00, 0x41, 0x7f, 0x40, 0x00},  // 'l'
    {0x7c, 0x04, 0x18, 0x04, 0x78},  // 'm'
    {0x7c, 0x08, 0x04, 0x04, 0x78},  // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38},  // 'o'
    {0x7c, 0x14, 0x14, 0x14, 0x08},  // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7c},  // 'q'
    {0x7c, 0x08, 0x04, 0x04, 0x08},  // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20},  // 's'
    {0x04, 0x3f, 0x44, 0x40, 0x20},  // 't'
    {0x3c, 0x40, 0x40, 0x20, 0x7c},  // 'u'
    {0x1c, 0x20, 0x40, 0x20, 0x1c},  // 'v'
    {0x3c, 0x40, 0x30, 0x40, 0x3c},  // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44},  // 'x'
    {0x0c, 0x50, 0x50, 0x50, 0x3c},  // 'y'
    {0x44, 0x64, 0x54, 0x4c, 0x44},  // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00},  // '{'
    {0x00, 0x00, 0x7f, 0x00, 0x00},  // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00},  // '}'
    {0x08, 0x04, 0x08, 0x10, 0x08},  // '~'
}};

enum class FontRole {
  Sans,
  Mono,
};

#if defined(CZ_HAS_FREETYPE)
struct CachedGlyph {
  int width = 0;
  int rows = 0;
  int left = 0;
  int top = 0;
  int advance_x = 0;
  std::vector<uint8_t> alpha;
};
#endif

// ---------------------------------------------------------------------------
// 320x170 Anti-Aliased Software Framebuffer + FreeType2 / 5x7 Fallback
// ---------------------------------------------------------------------------

class ShadcnCanvas {
 public:
  static constexpr int kW = AppView::kWidth;
  static constexpr int kH = AppView::kHeight;

  ShadcnCanvas() : buf_(static_cast<size_t>(kW * kH), 0xff09090bU) {
    init_fonts();
  }

  ~ShadcnCanvas() {
#if defined(CZ_HAS_FREETYPE)
    if (sans_face_ != nullptr) {
      FT_Done_Face(sans_face_);
      sans_face_ = nullptr;
    }
    if (mono_face_ != nullptr) {
      FT_Done_Face(mono_face_);
      mono_face_ = nullptr;
    }
    if (ft_lib_ != nullptr) {
      FT_Done_FreeType(ft_lib_);
      ft_lib_ = nullptr;
    }
#endif
  }

  bool using_freetype() const { return has_ft_sans_ || has_ft_mono_; }

  const uint32_t* pixels() const { return buf_.data(); }

  void clear(uint32_t rgb) {
    const uint32_t argb = 0xff000000U | (rgb & 0x00ffffffU);
    std::fill(buf_.begin(), buf_.end(), argb);
  }

  void dim_rect(int x, int y, int w, int h, uint32_t rgb, uint8_t alpha) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(kW, x + w);
    const int y1 = std::min(kH, y + h);
    for (int py = y0; py < y1; ++py) {
      for (int px = x0; px < x1; ++px) {
        blend_pixel(px, py, rgb, alpha);
      }
    }
  }

  void fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(kW, x + w);
    const int y1 = std::min(kH, y + h);
    const uint32_t argb = 0xff000000U | (rgb & 0x00ffffffU);
    for (int py = y0; py < y1; ++py) {
      for (int px = x0; px < x1; ++px) {
        buf_[static_cast<size_t>(py * kW + px)] = argb;
      }
    }
  }

  void draw_hline_alpha(int x, int y, int w, uint32_t rgb, uint8_t alpha) {
    if (y < 0 || y >= kH) return;
    const int x0 = std::max(0, x);
    const int x1 = std::min(kW, x + w);
    for (int px = x0; px < x1; ++px) {
      blend_pixel(px, y, rgb, alpha);
    }
  }

  void draw_vline_alpha(int x, int y, int h, uint32_t rgb, uint8_t alpha) {
    if (x < 0 || x >= kW) return;
    const int y0 = std::max(0, y);
    const int y1 = std::min(kH, y + h);
    for (int py = y0; py < y1; ++py) {
      blend_pixel(x, py, rgb, alpha);
    }
  }

  void draw_rgb24(int dst_x,
                  int dst_y,
                  int src_w,
                  int src_h,
                  const uint8_t* rgb24) {
    if (!rgb24 || src_w <= 0 || src_h <= 0) return;
    const int x0 = std::max(0, dst_x);
    const int y0 = std::max(0, dst_y);
    const int x1 = std::min(kW, dst_x + src_w);
    const int y1 = std::min(kH, dst_y + src_h);
    for (int py = y0; py < y1; ++py) {
      const int sy = py - dst_y;
      const uint8_t* srow = rgb24 + static_cast<size_t>(sy * src_w) * 3u;
      for (int px = x0; px < x1; ++px) {
        const int sx = px - dst_x;
        const uint8_t* sp = srow + static_cast<size_t>(sx) * 3u;
        buf_[static_cast<size_t>(py * kW + px)] =
            0xff000000U | (static_cast<uint32_t>(sp[0]) << 16) |
            (static_cast<uint32_t>(sp[1]) << 8) |
            static_cast<uint32_t>(sp[2]);
      }
    }
  }

  void draw_round_box(int x,
                      int y,
                      int w,
                      int h,
                      int radius,
                      uint32_t fill_rgb,
                      uint32_t border_rgb,
                      int border_w) {
    if (w <= 0 || h <= 0) {
      return;
    }
    const float r =
        static_cast<float>(std::max(0, std::min(radius, std::min(w, h) / 2)));
    const float hx = static_cast<float>(w) * 0.5f;
    const float hy = static_cast<float>(h) * 0.5f;
    const float cx = static_cast<float>(x) + hx;
    const float cy = static_cast<float>(y) + hy;
    const float bw = static_cast<float>(std::max(0, border_w));

    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(kW, x + w);
    const int y1 = std::min(kH, y + h);

    for (int py = y0; py < y1; ++py) {
      for (int px = x0; px < x1; ++px) {
        const float dx =
            std::abs((static_cast<float>(px) + 0.5f) - cx) - (hx - r);
        const float dy =
            std::abs((static_cast<float>(py) + 0.5f) - cy) - (hy - r);
        const float ox = std::max(dx, 0.0f);
        const float oy = std::max(dy, 0.0f);
        const float dist =
            std::hypot(ox, oy) + std::min(std::max(dx, dy), 0.0f) - r;

        const float outer = std::clamp(0.5f - dist, 0.0f, 1.0f);
        if (outer <= 0.001f) {
          continue;
        }
        if (bw <= 0.0f) {
          const uint8_t a = static_cast<uint8_t>(std::lround(outer * 255.0f));
          blend_pixel(px, py, fill_rgb, a);
          continue;
        }
        const float inner = std::clamp(0.5f - (dist + bw), 0.0f, 1.0f);
        const float edge = std::max(0.0f, outer - inner);
        if (inner > 0.001f) {
          const uint8_t ia = static_cast<uint8_t>(std::lround(inner * 255.0f));
          blend_pixel(px, py, fill_rgb, ia);
        }
        if (edge > 0.001f) {
          const uint8_t ba = static_cast<uint8_t>(std::lround(edge * 255.0f));
          blend_pixel(px, py, border_rgb, ba);
        }
      }
    }
  }

  void draw_circle(float cx,
                   float cy,
                   float radius,
                   uint32_t rgb,
                   bool hollow = false,
                   uint8_t max_alpha = 255) {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - radius - 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - radius - 1.0f)));
    const int x1 = std::min(kW, static_cast<int>(std::ceil(cx + radius + 1.0f)));
    const int y1 = std::min(kH, static_cast<int>(std::ceil(cy + radius + 1.0f)));
    for (int py = y0; py < y1; ++py) {
      for (int px = x0; px < x1; ++px) {
        const float d = std::hypot((static_cast<float>(px) + 0.5f) - cx,
                                   (static_cast<float>(py) + 0.5f) - cy);
        float alpha_f = 0.0f;
        if (hollow) {
          const float ring_d = std::abs(d - radius);
          alpha_f = std::clamp(0.75f - ring_d, 0.0f, 1.0f);
        } else {
          alpha_f = std::clamp(radius + 0.5f - d, 0.0f, 1.0f);
        }
        if (alpha_f > 0.01f) {
          const uint8_t a = static_cast<uint8_t>(
              std::lround(alpha_f * static_cast<float>(max_alpha)));
          blend_pixel(px, py, rgb, a);
        }
      }
    }
  }

  void draw_play_triangle(int cx, int cy, int half_h, uint32_t rgb) {
    const int left_x = cx - half_h / 2;
    const int width = half_h + 2;
    for (int col = 0; col <= width; ++col) {
      const float t = static_cast<float>(col) / static_cast<float>(width);
      const int span = static_cast<int>(std::lround((1.0f - t) * half_h));
      for (int dy = -span; dy <= span; ++dy) {
        blend_pixel(left_x + col, cy + dy, rgb, 245);
      }
    }
  }

  int measure_text(FontRole role, int px_size, const std::string& text) {
#if defined(CZ_HAS_FREETYPE)
    FT_Face face = select_face(role);
    if (face != nullptr) {
      int width = 0;
      for (unsigned char ch : text) {
        if (ch < 0x20 || ch > 0x7e) {
          ch = '?';
        }
        const auto* g = get_glyph(role, face, px_size, ch);
        width += (g != nullptr) ? g->advance_x : (px_size / 2);
      }
      return width;
    }
#else
    (void)role;
#endif
    const int scale = (px_size >= 14) ? 2 : 1;
    return static_cast<int>(text.size()) * 6 * scale;
  }

  std::string truncate_right(FontRole role,
                             int px_size,
                             const std::string& s,
                             int max_w) {
    if (max_w <= 0) return "";
    if (measure_text(role, px_size, s) <= max_w) return s;
    const std::string ell = "...";
    if (measure_text(role, px_size, ell) > max_w) return "";
    std::string cur = s;
    while (!cur.empty() &&
           measure_text(role, px_size, cur + ell) > max_w) {
      cur.pop_back();
    }
    return cur + ell;
  }

  std::string truncate_left_path(FontRole role,
                                 int px_size,
                                 const std::string& s,
                                 int max_w) {
    if (max_w <= 0) return "";
    if (measure_text(role, px_size, s) <= max_w) return s;
    const std::string ell = "...";
    if (measure_text(role, px_size, ell) > max_w) return "";
    size_t pos = 0;
    while (pos < s.size() &&
           measure_text(role, px_size, ell + s.substr(pos)) > max_w) {
      ++pos;
    }
    return ell + s.substr(pos);
  }

  void draw_text(FontRole role,
                 int px_size,
                 int x,
                 int y,
                 const std::string& text,
                 uint32_t rgb) {
#if defined(CZ_HAS_FREETYPE)
    FT_Face face = select_face(role);
    if (face != nullptr) {
      FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px_size));
      const int ascender = static_cast<int>(face->size->metrics.ascender >> 6);
      const int baseline_y = y + ascender;
      int pen_x = x;
      for (unsigned char ch : text) {
        if (ch < 0x20 || ch > 0x7e) {
          ch = '?';
        }
        const auto* g = get_glyph(role, face, px_size, ch);
        if (g == nullptr) {
          pen_x += px_size / 2;
          continue;
        }
        const int gx = pen_x + g->left;
        const int gy = baseline_y - g->top;
        for (int r = 0; r < g->rows; ++r) {
          for (int c = 0; c < g->width; ++c) {
            const uint8_t cov =
                g->alpha[static_cast<size_t>(r * g->width + c)];
            if (cov > 0) {
              blend_pixel(gx + c, gy + r, rgb, cov);
            }
          }
        }
        pen_x += g->advance_x;
      }
      return;
    }
#else
    (void)role;
#endif
    const int scale = (px_size >= 14) ? 2 : 1;
    int pen_x = x;
    for (unsigned char ch : text) {
      if (ch < 0x20 || ch > 0x7e) {
        ch = '?';
      }
      const auto& glyph = kFont5x7[static_cast<size_t>(ch - 0x20)];
      for (int col = 0; col < 5; ++col) {
        const uint8_t bits = glyph[static_cast<size_t>(col)];
        for (int row = 0; row < 7; ++row) {
          if ((bits & (1U << row)) != 0) {
            fill_rect(pen_x + col * scale, y + row * scale, scale, scale, rgb);
          }
        }
      }
      pen_x += 6 * scale;
    }
  }

 private:
  void init_fonts() {
#if defined(CZ_HAS_FREETYPE)
    if (FT_Init_FreeType(&ft_lib_) != 0) {
      ft_lib_ = nullptr;
      return;
    }

    auto try_load_list = [&](const std::vector<std::string>& paths,
                             FT_Face& out_face) -> bool {
      for (const auto& p : paths) {
        if (p.empty()) continue;
        if (FT_New_Face(ft_lib_, p.c_str(), 0, &out_face) == 0) {
          return true;
        }
      }
      return false;
    };

    std::vector<std::string> sans_candidates;
    if (const char* env_sans = std::getenv("CZCAMERA_FONT_SANS")) {
      sans_candidates.emplace_back(env_sans);
    } else {
      sans_candidates = {
          "/usr/share/czcamera/fonts/Inter-Regular.ttf",
          "/usr/share/czcamera/fonts/Inter.ttf",
          "assets/fonts/Inter-Regular.ttf",
          "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
          "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
          "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
          "/System/Library/Fonts/SFNS.ttf",
          "/System/Library/Fonts/Geneva.ttf",
          "/System/Library/Fonts/Helvetica.ttc",
      };
    }

    std::vector<std::string> mono_candidates;
    if (const char* env_mono = std::getenv("CZCAMERA_FONT_MONO")) {
      mono_candidates.emplace_back(env_mono);
    } else {
      mono_candidates = {
          "/usr/share/czcamera/fonts/JetBrainsMono-Regular.ttf",
          "/usr/share/czcamera/fonts/JetBrainsMono.ttf",
          "assets/fonts/JetBrainsMono-Regular.ttf",
          "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
          "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
          "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
          "/System/Library/Fonts/SFNSMono.ttf",
          "/System/Library/Fonts/Monaco.ttf",
          "/System/Library/Fonts/Menlo.ttc",
      };
    }

    has_ft_sans_ = try_load_list(sans_candidates, sans_face_);
    has_ft_mono_ = try_load_list(mono_candidates, mono_face_);
#endif
  }

#if defined(CZ_HAS_FREETYPE)
  FT_Face select_face(FontRole role) const {
    if (role == FontRole::Mono) {
      return mono_face_ != nullptr ? mono_face_ : sans_face_;
    }
    return sans_face_ != nullptr ? sans_face_ : mono_face_;
  }

  const CachedGlyph* get_glyph(FontRole role,
                               FT_Face face,
                               int px_size,
                               uint32_t codepoint) {
    const uint64_t key =
        (static_cast<uint64_t>(role == FontRole::Mono ? 1 : 0) << 48) |
        (static_cast<uint64_t>(px_size & 0xffff) << 32) |
        static_cast<uint64_t>(codepoint);
    const auto it = glyph_cache_.find(key);
    if (it != glyph_cache_.end()) {
      return &it->second;
    }

    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(px_size));
    if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0) {
      return nullptr;
    }

    FT_GlyphSlot slot = face->glyph;
    CachedGlyph cg;
    cg.width = static_cast<int>(slot->bitmap.width);
    cg.rows = static_cast<int>(slot->bitmap.rows);
    cg.left = slot->bitmap_left;
    cg.top = slot->bitmap_top;
    cg.advance_x = static_cast<int>(slot->advance.x >> 6);
    cg.alpha.resize(static_cast<size_t>(cg.width * cg.rows));

    if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
      for (int r = 0; r < cg.rows; ++r) {
        const uint8_t* src = slot->bitmap.buffer + r * slot->bitmap.pitch;
        std::memcpy(cg.alpha.data() + r * cg.width, src,
                    static_cast<size_t>(cg.width));
      }
    } else if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
      for (int r = 0; r < cg.rows; ++r) {
        const uint8_t* src = slot->bitmap.buffer + r * slot->bitmap.pitch;
        for (int c = 0; c < cg.width; ++c) {
          const uint8_t byte = src[c >> 3];
          const bool on = (byte & (0x80U >> (c & 7))) != 0;
          cg.alpha[static_cast<size_t>(r * cg.width + c)] = on ? 255 : 0;
        }
      }
    }

    auto [ins_it, inserted] = glyph_cache_.emplace(key, std::move(cg));
    (void)inserted;
    return &ins_it->second;
  }
#endif

  void blend_pixel(int x, int y, uint32_t fg_rgb, uint8_t alpha) {
    if (x < 0 || x >= kW || y < 0 || y >= kH || alpha == 0) {
      return;
    }
    const size_t idx = static_cast<size_t>(y * kW + x);
    if (alpha == 255) {
      buf_[idx] = 0xff000000U | (fg_rgb & 0x00ffffffU);
      return;
    }
    const uint32_t bg = buf_[idx];
    const uint32_t a = alpha;
    const uint32_t inv = 255U - a;
    const uint32_t fr = (fg_rgb >> 16) & 0xffU;
    const uint32_t fg = (fg_rgb >> 8) & 0xffU;
    const uint32_t fb = fg_rgb & 0xffU;
    const uint32_t br = (bg >> 16) & 0xffU;
    const uint32_t bg_c = (bg >> 8) & 0xffU;
    const uint32_t bb = bg & 0xffU;
    const uint32_t rr = (fr * a + br * inv + 127U) / 255U;
    const uint32_t rg = (fg * a + bg_c * inv + 127U) / 255U;
    const uint32_t rb = (fb * a + bb * inv + 127U) / 255U;
    buf_[idx] = 0xff000000U | (rr << 16) | (rg << 8) | rb;
  }

  std::vector<uint32_t> buf_;
  bool has_ft_sans_ = false;
  bool has_ft_mono_ = false;

#if defined(CZ_HAS_FREETYPE)
  FT_Library ft_lib_ = nullptr;
  FT_Face sans_face_ = nullptr;
  FT_Face mono_face_ = nullptr;
  std::map<uint64_t, CachedGlyph> glyph_cache_;
#endif
};

// ---------------------------------------------------------------------------
// Component drawing helpers
// ---------------------------------------------------------------------------

void draw_kbd_hint(ShadcnCanvas& canvas,
                   const ShadcnPalette& pal,
                   int& cursor_x,
                   int y,
                   const std::string& key_text,
                   const std::string& label,
                   int px_size = 9,
                   int gap_after = 6) {
  const int kw = canvas.measure_text(FontRole::Mono, px_size, key_text) + 5;
  const int chip_h = (px_size <= 8) ? 11 : 13;
  canvas.draw_round_box(cursor_x, y, kw, chip_h, 3, pal.card, pal.border, 1);
  canvas.draw_text(FontRole::Mono, px_size, cursor_x + 3, y + 1, key_text,
                   pal.foreground);
  cursor_x += kw + 2;
  canvas.draw_text(FontRole::Sans, px_size, cursor_x, y + 1, label,
                   pal.muted_foreground);
  cursor_x +=
      canvas.measure_text(FontRole::Sans, px_size, label) + gap_after;
}

int measure_pill_badge_w(ShadcnCanvas& canvas,
                         const std::string& text,
                         bool with_dot) {
  const int text_w = canvas.measure_text(FontRole::Sans, 9, text);
  return (with_dot ? 9 : 0) + text_w + 10;
}

void draw_pill_badge(ShadcnCanvas& canvas,
                     int x,
                     int y,
                     int h,
                     const std::string& text,
                     uint32_t bg_rgb,
                     uint32_t border_rgb,
                     uint32_t fg_rgb,
                     bool with_dot) {
  const int w = measure_pill_badge_w(canvas, text, with_dot);
  canvas.draw_round_box(x, y, w, h, h / 2, bg_rgb, border_rgb, 1);
  int tx = x + 5;
  if (with_dot) {
    canvas.draw_circle(static_cast<float>(x + 8),
                       static_cast<float>(y) + static_cast<float>(h) * 0.5f,
                       2.5f, fg_rgb, false);
    tx += 8;
  }
  canvas.draw_text(FontRole::Sans, 9, tx, y + (h - 10) / 2, text, fg_rgb);
}

struct RailCardSpec {
  std::string label;
  std::string value;
  uint32_t value_color = 0;
  bool custom_color = false;
};

}  // namespace

struct AppView::Impl {
  ThemeMode theme = ThemeMode::Dark;
  ShadcnPalette pal = get_palette(ThemeMode::Dark);
  ShadcnCanvas canvas;
  std::vector<uint8_t> scaled_vf;
  std::vector<uint8_t> scaled_gal;

#if defined(CP0_HAS_LVGL)
  lv_obj_t* screen = nullptr;
  lv_style_t style_bg{};
  lv_style_t style_card{};
  bool lvgl_styles_inited = false;

  void sync_lvgl_styles() {
    if (!lv_is_initialized()) {
      lv_init();
    }
    if (!lvgl_styles_inited) {
      lv_style_init(&style_bg);
      lv_style_init(&style_card);
      lvgl_styles_inited = true;
    }
    lv_style_set_bg_color(&style_bg, lv_color_hex(pal.background));
    lv_style_set_bg_color(&style_card, lv_color_hex(pal.card));
    lv_style_set_border_color(&style_card, lv_color_hex(pal.border));
    if (screen != nullptr) {
      lv_obj_report_style_change(&style_bg);
      lv_obj_report_style_change(&style_card);
    }
  }
#endif

  ~Impl() {
#if defined(CP0_HAS_LVGL)
    if (lvgl_styles_inited) {
      lv_style_reset(&style_bg);
      lv_style_reset(&style_card);
      lvgl_styles_inited = false;
    }
#endif
  }

  void render_viewfinder(const UiState& state,
                         const PreviewFrame* live_frame,
                         int vf_x) {
    constexpr int kVfW = 226;
    constexpr int kVfH = 170;
    canvas.fill_rect(vf_x, 0, kVfW, kVfH, pal.viewfinder_surround);

    const bool show_live_frame =
        state.backend_ready && !state.backend_paused && live_frame != nullptr &&
        !live_frame->rgb24.empty() && live_frame->width > 0 &&
        live_frame->height > 0;

    const bool is_16_9 =
        (state.mode == AppMode::Video) ||
        (state.mode == AppMode::Timelapse &&
         state.config.timelapse_resolution == "1080p") ||
        (show_live_frame && live_frame->is_16_9);

    const int feed_w = kVfW;
    const int feed_h = is_16_9 ? 127 : kVfH;
    const int feed_y = is_16_9 ? 21 : 0;

    if (show_live_frame) {
      scaled_vf.resize(static_cast<size_t>(feed_w) * static_cast<size_t>(feed_h) *
                       3u);
      const float loupe =
          (state.mode == AppMode::Photo && state.loupe_enabled) ? 2.0f : 1.0f;
      scale_crop_rgb24(live_frame->rgb24.data(), live_frame->width,
                       live_frame->height, scaled_vf.data(), feed_w, feed_h,
                       loupe);
      if (state.mode == AppMode::Photo && state.filter != FilterId::None) {
        apply_filter_rgb24(scaled_vf.data(), feed_w, feed_h, state.filter);
      }
      canvas.draw_rgb24(vf_x, feed_y, feed_w, feed_h, scaled_vf.data());
    } else {
      const std::string msg = state.backend_paused
                                  ? "Camera Paused"
                                  : (state.backend_ready ? "Waiting for frame..."
                                                         : "Camera Offline [R]");
      const int mw = canvas.measure_text(FontRole::Sans, 9, msg);
      canvas.draw_text(FontRole::Sans, 9, vf_x + (kVfW - mw) / 2, 79, msg,
                       pal.muted_foreground);
    }

    // Rule-of-thirds overlay
    if (state.config.grid_enabled) {
      const int vx1 = vf_x + feed_w / 3;
      const int vx2 = vf_x + (feed_w * 2) / 3;
      const int hy1 = feed_y + feed_h / 3;
      const int hy2 = feed_y + (feed_h * 2) / 3;
      canvas.draw_vline_alpha(vx1, feed_y, feed_h, 0xffffff, 72);
      canvas.draw_vline_alpha(vx2, feed_y, feed_h, 0xffffff, 72);
      canvas.draw_hline_alpha(vf_x, hy1, feed_w, 0xffffff, 72);
      canvas.draw_hline_alpha(vf_x, hy2, feed_w, 0xffffff, 72);
    }

    // Top-left active modifier / recording badge
    const int badge_y = is_16_9 ? 4 : 5;
    if (state.mode == AppMode::Video && state.video_recording) {
      const std::string rec = "REC " + format_mmss(state.video_elapsed_sec);
      draw_pill_badge(canvas, vf_x + 5, badge_y, 16, rec, pal.status_error_bg,
                      pal.status_error, pal.status_error, true);
    } else if (state.mode == AppMode::Timelapse && state.timelapse.running) {
      const std::string tl = "TL " + pad3(state.timelapse.shots_taken);
      draw_pill_badge(canvas, vf_x + 5, badge_y, 16, tl, pal.status_online_bg,
                      pal.status_online, pal.status_online, true);
    } else if (state.mode == AppMode::Photo && state.filter != FilterId::None) {
      const std::string flt = filter_display_name(state.filter);
      draw_pill_badge(canvas, vf_x + 5, badge_y, 16, flt,
                      pal.status_sleeping_bg, pal.status_sleeping,
                      pal.status_sleeping, false);
    }

    // Top-right Zoom / Loupe badge
    int right_edge = vf_x + kVfW - 5;
    if (state.mode == AppMode::Photo && state.loupe_enabled) {
      const std::string ltext = "LOUPE 2x";
      const int lw = measure_pill_badge_w(canvas, ltext, false);
      draw_pill_badge(canvas, right_edge - lw, badge_y, 16, ltext,
                      pal.status_sleeping_bg, pal.status_sleeping,
                      pal.status_sleeping, false);
      right_edge -= lw + 4;
    }
    if (state.zoom > 1.05f) {
      const std::string ztext = format_zoom_str(state.zoom);
      const int zw = measure_pill_badge_w(canvas, ztext, false);
      draw_pill_badge(canvas, right_edge - zw, badge_y, 16, ztext, pal.card,
                      pal.ring, pal.foreground, false);
    }

    // Self-timer countdown overlay
    if (state.mode == AppMode::Photo && state.timer_active) {
      canvas.draw_round_box(vf_x + 83, 55, 60, 54, 8, pal.card,
                            pal.status_sleeping, 2);
      const std::string num = std::to_string(state.timer_remaining_sec);
      const int nw = canvas.measure_text(FontRole::Mono, 18, num);
      canvas.draw_text(FontRole::Mono, 18, vf_x + 83 + (60 - nw) / 2, 62, num,
                       pal.status_sleeping);
      const std::string hint = "Esc cancel";
      const int hw = canvas.measure_text(FontRole::Sans, 8, hint);
      canvas.draw_text(FontRole::Sans, 8, vf_x + 83 + (60 - hw) / 2, 93, hint,
                       pal.muted_foreground);
    }

    // Capture flash white border
    if (state.capture_flash_active) {
      canvas.fill_rect(vf_x, 0, kVfW, 3, 0xffffff);
      canvas.fill_rect(vf_x, kVfH - 3, kVfW, 3, 0xffffff);
      canvas.fill_rect(vf_x, 0, 3, kVfH, 0xffffff);
      canvas.fill_rect(vf_x + kVfW - 3, 0, 3, kVfH, 0xffffff);
    }

    // Non-blocking bottom toast inside viewfinder
    if (state.modal == OverlayModal::None && !state.toast_message.empty()) {
      const uint32_t bg =
          state.toast_is_error ? pal.status_error_bg : pal.status_online_bg;
      const uint32_t fg =
          state.toast_is_error ? pal.status_error : pal.status_online;
      canvas.draw_round_box(vf_x + 6, 146, kVfW - 12, 18, 4, bg, fg, 1);
      canvas.draw_circle(static_cast<float>(vf_x + 15), 155.0f, 2.5f, fg,
                         false);
      const std::string fit =
          canvas.truncate_right(FontRole::Mono, 9, state.toast_message,
                                kVfW - 32);
      canvas.draw_text(FontRole::Mono, 9, vf_x + 22, 150, fit, fg);
    }
  }

  void render_control_rail(const UiState& state, int rail_x) {
    constexpr int kRailW = 94;
    const bool rail_left = (state.config.rail_side == "left");
    canvas.fill_rect(rail_x, 0, kRailW, AppView::kHeight, pal.background);

    // 1px divider border between viewfinder and rail
    const int div_x = rail_left ? (rail_x + kRailW - 1) : rail_x;
    canvas.fill_rect(div_x, 0, 1, AppView::kHeight, pal.border);

    const int inner_x = rail_left ? (rail_x + 4) : (rail_x + 5);
    constexpr int kInnerW = 85;

    // 1. Segmented TabsList Mode Switcher: 1P · 2V · 3T
    canvas.draw_round_box(inner_x, 4, kInnerW, 18, 5, pal.muted, pal.border, 1);
    const char* tabs[3] = {"1P", "2V", "3T"};
    constexpr int kSegW = 27;
    for (int i = 0; i < 3; ++i) {
      const int sx = inner_x + 2 + i * kSegW;
      const bool active = (static_cast<int>(state.mode) == i);
      if (active) {
        const uint32_t border =
            (pal.active_tab != pal.muted) ? pal.border : pal.active_tab;
        const int bw = (pal.active_tab != pal.muted) ? 1 : 0;
        canvas.draw_round_box(sx, 6, kSegW, 14, 4, pal.active_tab, border, bw);
      }
      const uint32_t fg =
          active ? pal.active_tab_foreground : pal.muted_foreground;
      const int tw = canvas.measure_text(FontRole::Sans, 9, tabs[i]);
      canvas.draw_text(FontRole::Sans, 9, sx + (kSegW - tw) / 2, 8, tabs[i],
                       fg);
    }

    // 2. 4 Focusable Telemetry / Setting Cards
    std::array<RailCardSpec, 4> cards;
    switch (state.mode) {
      case AppMode::Photo:
        cards[0] = {"RES", format_res_upper(state.config.photo_resolution), 0,
                    false};
        cards[1] = {"EV", format_ev_str(state.config.ev), 0, false};
        cards[2] = {"FLT", filter_display_name(state.filter),
                    pal.status_sleeping, state.filter != FilterId::None};
        cards[3] = {
            "TMR",
            state.config.photo_timer_sec > 0
                ? (std::to_string(state.config.photo_timer_sec) + "s")
                : "Off",
            pal.status_sleeping, state.config.photo_timer_sec > 0};
        break;
      case AppMode::Video:
        cards[0] = {"VID", state.config.video_resolution, 0, false};
        cards[1] = {"MIC", state.config.video_audio ? "ON" : "MUTE",
                    state.config.video_audio ? pal.status_online
                                             : pal.status_sleeping,
                    true};
        cards[2] = {"ZOOM", format_zoom_str(state.zoom), 0, false};
        cards[3] = {"FREE", format_bytes_short(state.free_bytes), 0, false};
        break;
      case AppMode::Timelapse:
        if (state.timelapse.running) {
          std::string shot_str = pad3(state.timelapse.shots_taken);
          if (state.timelapse.stop_mode == "shots" &&
              state.timelapse.stop_value > 0) {
            shot_str += "/" + std::to_string(state.timelapse.stop_value);
          }
          cards[0] = {"SHOT", shot_str, pal.status_online, true};
          cards[1] = {
              "NEXT",
              std::to_string(state.timelapse.next_shot_in_sec) + "s",
              0,
              false};
          cards[2] = {"TIME", format_mmss(state.timelapse.elapsed_sec), 0,
                      false};
          cards[3] = {"FREE", format_bytes_short(state.free_bytes), 0, false};
        } else {
          cards[0] = {
              "INT",
              format_interval_short(state.config.timelapse_interval_sec),
              0,
              false};
          cards[1] = {
              "OUT",
              state.config.timelapse_output_mode == "photos" ? "JPG" : "MP4",
              0,
              false};
          cards[2] = {"RES",
                      format_res_upper(state.config.timelapse_resolution), 0,
                      false};
          cards[3] = {
              "STOP",
              format_stop_short(state.config.timelapse_stop_mode,
                                state.config.timelapse_stop_value),
              0,
              false};
        }
        break;
    }

    for (int i = 0; i < 4; ++i) {
      const int cy = 25 + i * 29;
      const bool focused = (!state.timelapse.running && state.rail_cursor == i);
      canvas.draw_round_box(inner_x, cy, kInnerW, 26, 4,
                            focused ? pal.card : pal.muted,
                            focused ? pal.ring : pal.border, 1);
      if (focused) {
        canvas.fill_rect(inner_x + 2, cy + 5, 2, 16, pal.ring);
      }
      const int text_x = inner_x + (focused ? 7 : 5);
      canvas.draw_text(FontRole::Sans, 8, text_x, cy + 2, cards[i].label,
                       pal.muted_foreground);
      const uint32_t vcol =
          cards[i].custom_color ? cards[i].value_color : pal.foreground;
      const std::string vfit =
          canvas.truncate_right(FontRole::Mono, 10, cards[i].value,
                                kInnerW - (focused ? 10 : 8));
      canvas.draw_text(FontRole::Mono, 10, text_x, cy + 12, vfit, vcol);
    }

    // 3. Compact <kbd> Footer Hints
    const int fx = rail_left ? rail_x : (rail_x + 1);
    canvas.fill_rect(fx, 139, kRailW - 1, 1, pal.border);
    canvas.fill_rect(fx, 140, kRailW - 1, 30, pal.muted);

    int hx1 = inner_x;
    int hx2 = inner_x;
    if (state.mode == AppMode::Photo) {
      draw_kbd_hint(canvas, pal, hx1, 143, "Ent", "Shot", 8, 3);
      draw_kbd_hint(canvas, pal, hx1, 143, "8", "Gal", 8, 2);
      draw_kbd_hint(canvas, pal, hx2, 156, "5/7", "Zm", 8, 3);
      draw_kbd_hint(canvas, pal, hx2, 156, "S", "Set", 8, 2);
    } else if (state.mode == AppMode::Video) {
      if (state.video_recording) {
        draw_kbd_hint(canvas, pal, hx1, 143, "Esc", "Stop Rec", 8, 2);
        draw_kbd_hint(canvas, pal, hx2, 156, "5/7", "Zoom", 8, 2);
      } else {
        draw_kbd_hint(canvas, pal, hx1, 143, "Ent", "Rec", 8, 4);
        draw_kbd_hint(canvas, pal, hx1, 143, "M", "Mic", 8, 2);
        draw_kbd_hint(canvas, pal, hx2, 156, "5/7", "Zm", 8, 3);
        draw_kbd_hint(canvas, pal, hx2, 156, "S", "Set", 8, 2);
      }
    } else {
      if (state.timelapse.running) {
        draw_kbd_hint(canvas, pal, hx1, 143, "Esc", "Stop", 8, 2);
        canvas.draw_text(FontRole::Sans, 8, inner_x, 157, "Capturing...",
                         pal.status_online);
      } else {
        draw_kbd_hint(canvas, pal, hx1, 143, "Ent", "Go", 8, 4);
        draw_kbd_hint(canvas, pal, hx1, 143, "C", "Cust", 8, 2);
        draw_kbd_hint(canvas, pal, hx2, 156, "8", "Gal", 8, 4);
        draw_kbd_hint(canvas, pal, hx2, 156, "S", "Set", 8, 2);
      }
    }
  }

  void render_gallery(const UiState& state, const PreviewFrame* gallery_frame) {
    // Top bar (22 px)
    canvas.fill_rect(0, 0, AppView::kWidth, 22, pal.muted);
    canvas.fill_rect(0, 21, AppView::kWidth, 1, pal.border);

    const std::string title =
        state.gallery_in_session ? state.gallery_session_name : "Gallery";
    const int total = static_cast<int>(state.gallery_items.size());
    const int cur_num = (total > 0) ? (state.gallery_cursor + 1) : 0;
    const std::string idx_str =
        std::to_string(cur_num) + "/" + std::to_string(total);
    const std::string left_title =
        canvas.truncate_right(FontRole::Sans, 10, title, 105);
    canvas.draw_text(FontRole::Sans, 10, 6, 5, left_title, pal.foreground);
    const int tw = canvas.measure_text(FontRole::Sans, 10, left_title);
    canvas.draw_text(FontRole::Mono, 9, 10 + tw, 6, idx_str,
                     pal.muted_foreground);
    const int left_used = 10 + tw + canvas.measure_text(FontRole::Mono, 9, idx_str);

    if (total > 0 && state.gallery_cursor >= 0 &&
        state.gallery_cursor < total) {
      const auto& item =
          state.gallery_items[static_cast<size_t>(state.gallery_cursor)];
      std::string meta = item.filename;
      if (item.type == GalleryItemType::Photo) {
        if (item.width >= 3000) {
          meta += " | 8MP";
        } else if (item.width > 0) {
          meta += " | 2MP";
        }
        meta += " | " + format_bytes_short(item.size_bytes);
      } else if (item.type == GalleryItemType::Video) {
        if (item.duration_sec > 0.0) {
          meta += " | " + format_mmss(static_cast<int>(std::lround(item.duration_sec)));
        }
        meta += " | " + format_bytes_short(item.size_bytes);
      } else {
        meta += " | " + std::to_string(item.photo_count) + "p";
      }
      const int max_meta_w = std::max(40, AppView::kWidth - 8 - left_used - 6);
      const std::string meta_fit =
          canvas.truncate_left_path(FontRole::Mono, 9, meta, max_meta_w);
      const int mw = canvas.measure_text(FontRole::Mono, 9, meta_fit);
      canvas.draw_text(FontRole::Mono, 9, AppView::kWidth - 6 - mw, 6, meta_fit,
                       pal.muted_foreground);
    }

    // Center viewport (y=22..149, h=128)
    canvas.fill_rect(0, 22, AppView::kWidth, 128, pal.viewfinder_surround);
    if (total == 0) {
      const std::string empty = "No captures yet";
      const int ew = canvas.measure_text(FontRole::Sans, 11, empty);
      canvas.draw_text(FontRole::Sans, 11, (AppView::kWidth - ew) / 2, 72,
                       empty, pal.muted_foreground);
      const std::string sub = "Press [Esc] to return to camera";
      const int sw = canvas.measure_text(FontRole::Sans, 9, sub);
      canvas.draw_text(FontRole::Sans, 9, (AppView::kWidth - sw) / 2, 90, sub,
                       pal.muted_foreground);
    } else {
      if (gallery_frame != nullptr && !gallery_frame->rgb24.empty() &&
          gallery_frame->width > 0 && gallery_frame->height > 0) {
        int draw_w = gallery_frame->width;
        int draw_h = gallery_frame->height;
        const uint8_t* draw_ptr = gallery_frame->rgb24.data();
        if (draw_w > AppView::kWidth || draw_h > 128) {
          if (media_detail::aspect_scale_rgb24(draw_ptr, draw_w, draw_h,
                                               AppView::kWidth, 128, scaled_gal,
                                               draw_w, draw_h)) {
            draw_ptr = scaled_gal.data();
          }
        }
        const int dx = (AppView::kWidth - draw_w) / 2;
        const int dy = 22 + (128 - draw_h) / 2;
        canvas.draw_rgb24(dx, dy, draw_w, draw_h, draw_ptr);
      } else {
        const std::string noprev = "Preview unavailable";
        const int nw = canvas.measure_text(FontRole::Sans, 9, noprev);
        canvas.draw_text(FontRole::Sans, 9, (AppView::kWidth - nw) / 2, 80,
                         noprev, pal.muted_foreground);
      }

      const auto& item =
          state.gallery_items[static_cast<size_t>(state.gallery_cursor)];
      if (item.type == GalleryItemType::Video) {
        if (!state.video_player.playing) {
          canvas.draw_circle(160.0f, 80.0f, 15.0f, pal.background, false, 185);
          canvas.draw_circle(160.0f, 80.0f, 15.0f, pal.ring, true, 220);
          canvas.draw_play_triangle(159, 80, 7, pal.foreground);
        }

        // Seek progress bar + time pill
        const double dur =
            std::max(state.video_player.duration_sec, item.duration_sec);
        const double frac =
            (dur > 0.01)
                ? std::clamp(state.video_player.position_sec / dur, 0.0, 1.0)
                : 0.0;
        const std::string tstr =
            format_mmss(static_cast<int>(std::floor(state.video_player.position_sec))) +
            " / " + format_mmss(static_cast<int>(std::lround(dur)));
        draw_pill_badge(canvas, 8, 122, 15, tstr, pal.card, pal.border,
                        pal.foreground, false);
        if (state.video_player.muted) {
          const std::string mtxt = "MUTE";
          const int mw = measure_pill_badge_w(canvas, mtxt, false);
          draw_pill_badge(canvas, AppView::kWidth - 8 - mw, 122, 15, mtxt,
                          pal.status_sleeping_bg, pal.status_sleeping,
                          pal.status_sleeping, false);
        }

        canvas.draw_round_box(8, 141, 304, 5, 2, pal.muted, pal.border, 1);
        const int fill_w =
            static_cast<int>(std::lround(frac * 302.0));
        if (fill_w > 0) {
          canvas.fill_rect(9, 142, fill_w, 3, pal.status_online);
        }
      } else if (item.type == GalleryItemType::TimelapseFolder) {
        const std::string tag =
            "FOLDER | " + std::to_string(item.photo_count) +
            " photos | [Ent] Open";
        const int tw2 = measure_pill_badge_w(canvas, tag, false);
        draw_pill_badge(canvas, (AppView::kWidth - tw2) / 2, 128, 16, tag,
                        pal.card, pal.ring, pal.foreground, false);
      }
    }

    if (!state.toast_message.empty()) {
      const uint32_t bg =
          state.toast_is_error ? pal.status_error_bg : pal.status_online_bg;
      const uint32_t fg =
          state.toast_is_error ? pal.status_error : pal.status_online;
      canvas.draw_round_box(16, 26, 288, 17, 4, bg, fg, 1);
      const std::string fit =
          canvas.truncate_right(FontRole::Mono, 9, state.toast_message, 274);
      canvas.draw_text(FontRole::Mono, 9, 23, 30, fit, fg);
    }

    // Bottom bar (y=150..169)
    if (state.gallery_delete_confirm && total > 0) {
      const auto& item =
          state.gallery_items[static_cast<size_t>(state.gallery_cursor)];
      canvas.fill_rect(0, 150, AppView::kWidth, 20, pal.status_error_bg);
      canvas.fill_rect(0, 150, AppView::kWidth, 1, pal.status_error);
      std::string prompt;
      if (item.type == GalleryItemType::TimelapseFolder) {
        prompt = "Delete folder (" + std::to_string(item.photo_count) +
                 " photos)? [Y] Del  [N] Cancel";
      } else {
        prompt = "Delete " +
                 canvas.truncate_right(FontRole::Mono, 9, item.filename, 110) +
                 "? [Y] Del  [N] Cancel";
      }
      const std::string pfit =
          canvas.truncate_right(FontRole::Mono, 9, prompt, 308);
      canvas.draw_text(FontRole::Mono, 9, 6, 155, pfit, pal.status_error);
      return;
    }

    canvas.fill_rect(0, 150, AppView::kWidth, 20, pal.muted);
    canvas.fill_rect(0, 150, AppView::kWidth, 1, pal.border);
    int x = 6;
    const int y = 154;
    if (total > 0) {
      const auto& item =
          state.gallery_items[static_cast<size_t>(state.gallery_cursor)];
      if (item.type == GalleryItemType::Video) {
        draw_kbd_hint(canvas, pal, x, y, "</>", "Nav", 9, 4);
        draw_kbd_hint(canvas, pal, x, y, "Spc",
                      state.video_player.playing ? "Pause" : "Play", 9, 4);
        draw_kbd_hint(canvas, pal, x, y, "M", "Mute", 9, 4);
        draw_kbd_hint(canvas, pal, x, y, "D", "Del", 9, 4);
        draw_kbd_hint(canvas, pal, x, y, "Esc", "Back", 9, 2);
      } else if (item.type == GalleryItemType::TimelapseFolder) {
        draw_kbd_hint(canvas, pal, x, y, "</>", "Nav", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "Ent", "Open", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "D", "Del", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "Esc", "Back", 9, 2);
      } else if (state.gallery_in_session) {
        draw_kbd_hint(canvas, pal, x, y, "</>", "Nav", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "D", "Del", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "Bksp", "Root", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "Esc", "Back", 9, 2);
      } else {
        draw_kbd_hint(canvas, pal, x, y, "</>", "Nav", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "D", "Del", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "T", "Theme", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "Esc", "Back", 9, 2);
      }
    } else {
      draw_kbd_hint(canvas, pal, x, y, "T", "Theme", 9, 6);
      draw_kbd_hint(canvas, pal, x, y, "Esc", "Back to Camera", 9, 4);
    }
  }

  void render_modal(const UiState& state) {
    if (state.modal == OverlayModal::None ||
        state.modal == OverlayModal::Gallery) {
      return;
    }

    canvas.dim_rect(0, 0, AppView::kWidth, 150, 0x000000, 155);

    if (state.modal == OverlayModal::Settings) {
      canvas.draw_round_box(12, 8, 296, 138, 6, pal.card, pal.ring, 1);
      canvas.draw_text(FontRole::Sans, 10, 20, 13, "Settings", pal.foreground);
      const std::string esc_hint = "[Esc] Save";
      const int ew = canvas.measure_text(FontRole::Mono, 9, esc_hint);
      canvas.draw_text(FontRole::Mono, 9, 300 - ew, 14, esc_hint,
                       pal.muted_foreground);
      canvas.fill_rect(18, 27, 284, 1, pal.border);

      struct SettingRow {
        std::string label;
        std::string value;
        bool is_path = false;
      };
      const std::string dim_val =
          state.config.timelapse_dim_sec > 0
              ? (std::to_string(state.config.timelapse_dim_sec) + "s")
              : "Off";
      const std::array<SettingRow, 6> rows = {{
          {"Output Dir", state.config.output_dir, true},
          {"Control Rail",
           state.config.rail_side == "left" ? "Left" : "Right", false},
          {"Theme", state.theme == ThemeMode::Light ? "Light" : "Dark", false},
          {"Grid Overlay", state.config.grid_enabled ? "On" : "Off", false},
          {"Timelapse Dim", dim_val, false},
          {"Camera Orient",
           format_orient_display(state.config.camera_transform), false},
      }};

      for (int i = 0; i < 6; ++i) {
        const int ry = 30 + i * 19;
        const bool sel = (state.settings_cursor == i);
        canvas.draw_round_box(18, ry, 284, 17, 4,
                              sel ? pal.muted : pal.card,
                              sel ? pal.ring : pal.border, 1);
        canvas.draw_text(FontRole::Sans, 9, 24, ry + 4, rows[i].label,
                         sel ? pal.foreground : pal.muted_foreground);
        const std::string val_fit =
            rows[i].is_path
                ? canvas.truncate_left_path(FontRole::Mono, 9, rows[i].value,
                                            175)
                : canvas.truncate_right(FontRole::Mono, 9, rows[i].value, 140);
        const int vw = canvas.measure_text(FontRole::Mono, 9, val_fit);
        canvas.draw_text(FontRole::Mono, 9, 296 - vw, ry + 4, val_fit,
                         pal.foreground);
      }
    } else if (state.modal == OverlayModal::DirPicker) {
      canvas.draw_round_box(12, 12, 296, 134, 6, pal.card, pal.ring, 1);
      canvas.draw_text(FontRole::Sans, 10, 20, 17, "Select Output Directory",
                       pal.foreground);
      const std::string manual_hint = "[T] Manual";
      const int hw = canvas.measure_text(FontRole::Mono, 9, manual_hint);
      canvas.draw_text(FontRole::Mono, 9, 300 - hw, 18, manual_hint,
                       pal.muted_foreground);

      canvas.draw_round_box(18, 32, 284, 16, 3, pal.muted, pal.border, 1);
      const std::string cur_fit = canvas.truncate_left_path(
          FontRole::Mono, 9, state.dir_current_path, 272);
      canvas.draw_text(FontRole::Mono, 9, 24, 35, cur_fit, pal.foreground);

      if (state.dir_entries.empty()) {
        canvas.draw_text(FontRole::Sans, 9, 22, 72,
                         "(No subdirs - Space selects current dir)",
                         pal.muted_foreground);
      } else {
        constexpr int kRows = 4;
        const int total = static_cast<int>(state.dir_entries.size());
        int start = 0;
        if (state.dir_cursor >= kRows) {
          start = state.dir_cursor - kRows + 1;
        }
        if (start + kRows > total) {
          start = std::max(0, total - kRows);
        }
        for (int r = 0; r < kRows && (start + r) < total; ++r) {
          const int idx = start + r;
          const bool sel = (idx == state.dir_cursor);
          const int ry = 52 + r * 22;
          canvas.draw_round_box(18, ry, 284, 19, 4,
                                sel ? pal.muted : pal.card,
                                sel ? pal.ring : pal.border, 1);
          const std::string& full =
              state.dir_entries[static_cast<size_t>(idx)];
          const size_t slash = full.rfind('/');
          const std::string name =
              (slash != std::string::npos) ? full.substr(slash + 1) : full;
          const std::string fit =
              canvas.truncate_right(FontRole::Mono, 9, "> " + name + "/", 268);
          canvas.draw_text(FontRole::Mono, 9, 24, ry + 5, fit, pal.foreground);
        }
      }
    } else if (state.modal == OverlayModal::TextInput) {
      canvas.draw_round_box(18, 28, 284, 96, 6, pal.card, pal.ring, 1);
      canvas.draw_text(FontRole::Sans, 10, 28, 38, "Enter Output Directory",
                       pal.foreground);
      canvas.draw_round_box(28, 56, 264, 24, 4, pal.muted, pal.ring, 1);
      const std::string shown = canvas.truncate_left_path(
          FontRole::Mono, 10, state.text_buffer + "_", 250);
      canvas.draw_text(FontRole::Mono, 10, 35, 63, shown, pal.foreground);

      if (!state.modal_error.empty()) {
        const std::string err =
            canvas.truncate_right(FontRole::Sans, 9, state.modal_error, 264);
        canvas.draw_text(FontRole::Sans, 9, 28, 90, err, pal.status_error);
      } else {
        canvas.draw_text(FontRole::Mono, 9, 28, 90,
                         "Example: ~/Pictures/czcamera", pal.muted_foreground);
      }
    } else if (state.modal == OverlayModal::TimelapseCustomInput) {
      canvas.draw_round_box(18, 16, 284, 124, 6, pal.card, pal.ring, 1);
      canvas.draw_text(FontRole::Sans, 10, 26, 22, "Timelapse Setup",
                       pal.foreground);
      canvas.fill_rect(24, 36, 272, 1, pal.border);

      std::string stop_val_str = "Off";
      if (state.tl_custom_stop_mode == "shots") {
        stop_val_str = std::to_string(state.tl_custom_stop_value) + " shots";
      } else if (state.tl_custom_stop_mode == "duration") {
        stop_val_str =
            std::to_string(std::max(1, state.tl_custom_stop_value / 60)) + " min";
      }

      const std::array<std::pair<std::string, std::string>, 4> rows = {{
          {"Interval (s)",
           std::to_string(state.tl_custom_interval_sec) + "s"},
          {"Video FPS", std::to_string(state.tl_custom_fps) + " fps"},
          {"Stop Mode", state.tl_custom_stop_mode},
          {"Stop Value", stop_val_str},
      }};
      for (int i = 0; i < 4; ++i) {
        const int ry = 42 + i * 22;
        const bool sel = (state.tl_custom_cursor == i);
        canvas.draw_round_box(24, ry, 272, 19, 4,
                              sel ? pal.muted : pal.card,
                              sel ? pal.ring : pal.border, 1);
        canvas.draw_text(FontRole::Sans, 9, 30, ry + 5, rows[i].first,
                         sel ? pal.foreground : pal.muted_foreground);
        const int vw =
            canvas.measure_text(FontRole::Mono, 9, rows[i].second);
        canvas.draw_text(FontRole::Mono, 9, 290 - vw, ry + 5, rows[i].second,
                         pal.foreground);
      }
    } else if (state.modal == OverlayModal::TimelapseRecovery) {
      canvas.draw_round_box(16, 22, 288, 114, 6, pal.card, pal.ring, 1);
      canvas.draw_text(FontRole::Sans, 10, 26, 30, "Unfinished Timelapse Found",
                       pal.status_sleeping);
      canvas.fill_rect(24, 46, 272, 1, pal.border);

      const std::string line1 =
          "Session: " +
          canvas.truncate_right(FontRole::Mono, 9,
                                state.recovery_manifest.session_id, 185);
      const std::string line2 =
          "Frames: " + std::to_string(state.recovery_manifest.frame_count) +
          " (" + format_res_upper(state.recovery_manifest.resolution) + ")";
      canvas.draw_text(FontRole::Mono, 9, 26, 54, line1, pal.foreground);
      canvas.draw_text(FontRole::Mono, 9, 26, 70, line2, pal.muted_foreground);
      canvas.draw_text(FontRole::Mono, 9, 26, 94,
                       "[C] Compile MP4   [K] Keep Photos", pal.status_online);
      canvas.draw_text(FontRole::Mono, 9, 26, 110,
                       "[D] Discard       [Esc] Later", pal.status_error);
    } else if (state.modal == OverlayModal::TimelapseStopConfirm) {
      canvas.draw_round_box(26, 32, 268, 92, 6, pal.card, pal.ring, 1);
      canvas.draw_text(FontRole::Sans, 10, 38, 42, "Stop Active Timelapse?",
                       pal.foreground);
      const std::string info =
          "Shots: " + std::to_string(state.timelapse.shots_taken) +
          "  Elapsed: " + format_mmss(state.timelapse.elapsed_sec);
      canvas.draw_text(FontRole::Mono, 9, 38, 62, info, pal.muted_foreground);
      const std::string out_line =
          (state.config.timelapse_output_mode == "photos")
              ? "Output: Keep Photos Folder"
              : "Output: Compile MP4 Video";
      canvas.draw_text(FontRole::Sans, 9, 38, 78, out_line, pal.status_online);
      canvas.draw_text(FontRole::Mono, 9, 38, 100,
                       "[Y] Stop & Finish   [N / Esc] Resume", pal.foreground);
    } else if (state.modal == OverlayModal::TimelapseCompiling) {
      canvas.draw_round_box(22, 32, 276, 92, 6, pal.card, pal.ring, 1);
      canvas.draw_text(FontRole::Sans, 10, 34, 42, "Compiling Timelapse MP4",
                       pal.foreground);
      const std::string prog =
          "Compiling " + std::to_string(state.timelapse.compile_current) + "/" +
          std::to_string(state.timelapse.compile_total) + " frames...";
      canvas.draw_text(FontRole::Mono, 9, 34, 62, prog, pal.muted_foreground);

      canvas.draw_round_box(34, 80, 252, 12, 4, pal.muted, pal.border, 1);
      const int fw = static_cast<int>(
          std::lround(std::clamp(state.timelapse.compile_progress, 0.0f, 1.0f) *
                      250.0f));
      if (fw > 0) {
        canvas.fill_rect(35, 81, fw, 10, pal.status_online);
      }
      canvas.draw_text(FontRole::Mono, 9, 34, 102,
                       "[Esc] Cancel & Keep Photos", pal.muted_foreground);
    }

    // Modal footer bar
    canvas.fill_rect(0, 150, AppView::kWidth, 20, pal.muted);
    canvas.fill_rect(0, 150, AppView::kWidth, 1, pal.border);
    int x = 6;
    const int y = 154;
    switch (state.modal) {
      case OverlayModal::Settings:
        draw_kbd_hint(canvas, pal, x, y, "j/k", "Move");
        draw_kbd_hint(canvas, pal, x, y, "Ent/</>", "Change");
        draw_kbd_hint(canvas, pal, x, y, "Esc", "Close");
        break;
      case OverlayModal::DirPicker:
        draw_kbd_hint(canvas, pal, x, y, "Ent", "Open", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "Bksp", "Up", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "Spc", "Select", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "T", "Type", 9, 2);
        break;
      case OverlayModal::TextInput:
        draw_kbd_hint(canvas, pal, x, y, "Bksp", "Del");
        draw_kbd_hint(canvas, pal, x, y, "Ent", "Save");
        draw_kbd_hint(canvas, pal, x, y, "Esc", "Cancel");
        break;
      case OverlayModal::TimelapseCustomInput:
        draw_kbd_hint(canvas, pal, x, y, "j/k", "Row", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "</>/0-9", "Edit", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "Ent", "Apply", 9, 5);
        draw_kbd_hint(canvas, pal, x, y, "Esc", "Back", 9, 2);
        break;
      case OverlayModal::TimelapseRecovery:
        draw_kbd_hint(canvas, pal, x, y, "C", "Compile");
        draw_kbd_hint(canvas, pal, x, y, "K", "Keep");
        draw_kbd_hint(canvas, pal, x, y, "D", "Discard");
        draw_kbd_hint(canvas, pal, x, y, "Esc", "Later");
        break;
      case OverlayModal::TimelapseStopConfirm:
        draw_kbd_hint(canvas, pal, x, y, "Y", "Stop & Finish");
        draw_kbd_hint(canvas, pal, x, y, "N/Esc", "Resume");
        break;
      case OverlayModal::TimelapseCompiling:
        draw_kbd_hint(canvas, pal, x, y, "Esc", "Cancel & Keep Photos");
        break;
      case OverlayModal::None:
      case OverlayModal::Gallery:
        break;
    }
  }

  void render(const UiState& state,
              const PreviewFrame* live_frame,
              const PreviewFrame* gallery_frame) {
    if (state.theme != theme) {
      theme = state.theme;
      pal = get_palette(theme);
    }

    if (state.timelapse_dimmed && !state.has_hardware_backlight) {
      canvas.clear(0x000000);
      return;
    }

    canvas.clear(pal.background);

    if (state.modal == OverlayModal::Gallery) {
      render_gallery(state, gallery_frame);
    } else {
      const bool rail_left = (state.config.rail_side == "left");
      const int rail_x = rail_left ? 0 : 226;
      const int vf_x = rail_left ? 94 : 0;
      render_viewfinder(state, live_frame, vf_x);
      render_control_rail(state, rail_x);
      render_modal(state);
    }

#if defined(CP0_HAS_LVGL)
    sync_lvgl_styles();
#endif
  }
};

AppView::AppView() : impl_(std::make_unique<Impl>()) {}
AppView::~AppView() = default;

void AppView::apply_theme(ThemeMode mode) {
  impl_->theme = mode;
  impl_->pal = get_palette(mode);
#if defined(CP0_HAS_LVGL)
  impl_->sync_lvgl_styles();
#endif
}

void AppView::render(const UiState& state,
                     const PreviewFrame* live_frame,
                     const PreviewFrame* gallery_frame) {
  impl_->render(state, live_frame, gallery_frame);
}

const uint32_t* AppView::pixels() const { return impl_->canvas.pixels(); }

bool AppView::save_ppm(const std::string& path) const {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out << "P6\n" << kWidth << " " << kHeight << "\n255\n";
  const uint32_t* px = pixels();
  std::vector<uint8_t> rgb(static_cast<size_t>(kWidth * kHeight * 3));
  for (int i = 0; i < kWidth * kHeight; ++i) {
    const uint32_t c = px[i];
    rgb[static_cast<size_t>(i * 3 + 0)] = static_cast<uint8_t>((c >> 16) & 0xffU);
    rgb[static_cast<size_t>(i * 3 + 1)] = static_cast<uint8_t>((c >> 8) & 0xffU);
    rgb[static_cast<size_t>(i * 3 + 2)] = static_cast<uint8_t>(c & 0xffU);
  }
  out.write(reinterpret_cast<const char*>(rgb.data()),
            static_cast<std::streamsize>(rgb.size()));
  return out.good();
}

bool AppView::using_freetype() const {
  return impl_->canvas.using_freetype();
}

ThemeMode AppView::active_theme() const { return impl_->theme; }

}  // namespace cz
