#include "filters.hpp"
#include "jpeg_pipeline.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TempDir {
  std::string path;
  explicit TempDir(const std::string& name) {
    const std::string base =
        "/tmp/czcamera_test_" + name + "_" + std::to_string(::getpid());
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    assert(!ec);
    path = base;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void write_bytes(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  assert(out.is_open());
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  assert(out.good());
}

std::string read_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  assert(in.is_open());
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

bool is_dmg_olive(uint8_t r, uint8_t g, uint8_t b) {
  return (r == 0x0f && g == 0x38 && b == 0x0f) ||
         (r == 0x30 && g == 0x62 && b == 0x30) ||
         (r == 0x8b && g == 0xac && b == 0x0f) ||
         (r == 0x9b && g == 0xbc && b == 0x0f);
}

bool is_near_dmg_olive(uint8_t r, uint8_t g, uint8_t b, int tol = 28) {
  const uint8_t palette[4][3] = {
      {0x0f, 0x38, 0x0f},
      {0x30, 0x62, 0x30},
      {0x8b, 0xac, 0x0f},
      {0x9b, 0xbc, 0x0f},
  };
  for (const auto& c : palette) {
    if (std::abs(static_cast<int>(r) - c[0]) <= tol &&
        std::abs(static_cast<int>(g) - c[1]) <= tol &&
        std::abs(static_cast<int>(b) - c[2]) <= tol) {
      return true;
    }
  }
  return false;
}

void test_filter_cycle_and_serialization() {
  struct Case {
    cz::FilterId id;
    const char* serialized;
  };
  const Case cases[] = {
      {cz::FilterId::None, "none"},       {cz::FilterId::BW, "bw"},
      {cz::FilterId::HiCon, "hicon"},     {cz::FilterId::Sepia, "sepia"},
      {cz::FilterId::Cool, "cool"},       {cz::FilterId::Vivid, "vivid"},
      {cz::FilterId::Invert, "invert"},   {cz::FilterId::Retro, "retro"},
      {cz::FilterId::GameBoy, "gameboy"},
  };

  for (const auto& c : cases) {
    assert(cz::filter_to_string(c.id) == c.serialized);
    assert(cz::filter_from_string(c.serialized) == c.id);
    assert(!cz::filter_display_name(c.id).empty());
  }

  assert(cz::filter_from_string("invalid_filter") == cz::FilterId::None);
  assert(cz::filter_from_string("") == cz::FilterId::None);

  // Forward cycle and wrap-around
  cz::FilterId cur = cz::FilterId::None;
  for (int i = 0; i < 9; ++i) {
    assert(cur == cases[i].id);
    cur = cz::next_filter(cur, 1);
  }
  assert(cur == cz::FilterId::None);

  // Backward wrap-around
  assert(cz::next_filter(cz::FilterId::None, -1) == cz::FilterId::GameBoy);
  assert(cz::next_filter(cz::FilterId::GameBoy, 1) == cz::FilterId::None);
  assert(cz::next_filter(cz::FilterId::Sepia, 9) == cz::FilterId::Sepia);
  assert(cz::next_filter(cz::FilterId::Sepia, -9) == cz::FilterId::Sepia);
}

void test_all_nine_filters_pixel_behavior() {
  // Base 4x1 test pixels: mid-dark, mid-light, warm-ish color, saturated color
  const std::vector<uint8_t> base = {
      70,  70,  70,   // pixel 0: mid-dark gray
      185, 185, 185,  // pixel 1: mid-light gray
      180, 110, 90,   // pixel 2: warm midtone
      128, 128, 128,  // pixel 3: neutral gray
  };

  // 1. None: exact no-op
  {
    auto buf = base;
    cz::apply_filter_rgb24(buf.data(), 4, 1, cz::FilterId::None);
    assert(buf == base);
  }

  // 2. BW: R == G == B
  std::vector<uint8_t> bw_buf = base;
  cz::apply_filter_rgb24(bw_buf.data(), 4, 1, cz::FilterId::BW);
  for (int i = 0; i < 4; ++i) {
    const uint8_t r = bw_buf[i * 3 + 0];
    const uint8_t g = bw_buf[i * 3 + 1];
    const uint8_t b = bw_buf[i * 3 + 2];
    assert(r == g && g == b);
  }
  assert(bw_buf[0] < bw_buf[3]);  // dark < light

  // 3. HiCon: pushes mid-dark lower and mid-light higher than BW
  {
    auto buf = base;
    cz::apply_filter_rgb24(buf.data(), 4, 1, cz::FilterId::HiCon);
    for (int i = 0; i < 4; ++i) {
      assert(buf[i * 3 + 0] == buf[i * 3 + 1] &&
             buf[i * 3 + 1] == buf[i * 3 + 2]);
    }
    assert(buf[0] < bw_buf[0]);
    assert(buf[3] > bw_buf[3]);
  }

  // 4. Sepia: warm R >= G >= B and R > B on neutral gray
  {
    auto buf = base;
    cz::apply_filter_rgb24(buf.data(), 4, 1, cz::FilterId::Sepia);
    for (int i = 0; i < 4; ++i) {
      const uint8_t r = buf[i * 3 + 0];
      const uint8_t g = buf[i * 3 + 1];
      const uint8_t b = buf[i * 3 + 2];
      assert(r >= g && g >= b);
      assert(r > b);
    }
  }

  // 5. Cool: boosts B over R on neutral gray
  {
    auto buf = base;
    cz::apply_filter_rgb24(buf.data(), 4, 1, cz::FilterId::Cool);
    assert(buf[3 * 3 + 2] > buf[3 * 3 + 0]);  // neutral gray: B > R
    assert(buf[0 * 3 + 2] > buf[0 * 3 + 0]);
  }

  // 6. Vivid: widens chroma spread on colorful pixel
  {
    auto buf = base;
    cz::apply_filter_rgb24(buf.data(), 4, 1, cz::FilterId::Vivid);
    const int orig_spread =
        std::max({base[6], base[7], base[8]}) - std::min({base[6], base[7], base[8]});
    const int vivid_spread =
        std::max({buf[6], buf[7], buf[8]}) - std::min({buf[6], buf[7], buf[8]});
    assert(vivid_spread > orig_spread);
  }

  // 7. Invert: 255 - c
  {
    auto buf = base;
    cz::apply_filter_rgb24(buf.data(), 4, 1, cz::FilterId::Invert);
    for (size_t i = 0; i < buf.size(); ++i) {
      assert(buf[i] == static_cast<uint8_t>(255 - base[i]));
    }
  }

  // 8. Retro: deterministic across runs, lifted shadows, subtle edge vignette
  {
    const int w = 64;
    const int h = 48;
    std::vector<uint8_t> zeros(w * h * 3, 0);
    cz::apply_filter_rgb24(zeros.data(), w, h, cz::FilterId::Retro);
    // Center pixel shadow lifted above 0
    const size_t center_idx = ((h / 2) * w + (w / 2)) * 3;
    assert(zeros[center_idx] > 0 || zeros[center_idx + 1] > 0 ||
           zeros[center_idx + 2] > 0);

    std::vector<uint8_t> gray1(w * h * 3, 140);
    std::vector<uint8_t> gray2(w * h * 3, 140);
    cz::apply_filter_rgb24(gray1.data(), w, h, cz::FilterId::Retro);
    cz::apply_filter_rgb24(gray2.data(), w, h, cz::FilterId::Retro);
    assert(gray1 == gray2);  // deterministic

    // Center brighter than corner due to vignette
    const int corner_sum = gray1[0] + gray1[1] + gray1[2];
    const int center_sum =
        gray1[center_idx] + gray1[center_idx + 1] + gray1[center_idx + 2];
    assert(center_sum > corner_sum);
  }

  // 9. GameBoy: strictly 4 DMG olive colors + uniform max(1, width/160) blocks
  {
    // 226x170 viewfinder check (block = 1)
    const int vw = 226;
    const int vh = 170;
    std::vector<uint8_t> vf(vw * vh * 3);
    for (int y = 0; y < vh; ++y) {
      for (int x = 0; x < vw; ++x) {
        const uint8_t v = static_cast<uint8_t>((x * 255) / (vw - 1));
        const size_t idx = (static_cast<size_t>(y) * vw + x) * 3;
        vf[idx + 0] = v;
        vf[idx + 1] = v;
        vf[idx + 2] = v;
      }
    }
    cz::apply_filter_rgb24(vf.data(), vw, vh, cz::FilterId::GameBoy);
    for (int i = 0; i < vw * vh; ++i) {
      assert(is_dmg_olive(vf[i * 3 + 0], vf[i * 3 + 1], vf[i * 3 + 2]));
    }

    // 1600x1200 check: block = 1600 / 160 = 10 -> identical pixels inside 10x10 block
    const int hw = 1600;
    const int hh = 1200;
    std::vector<uint8_t> hi(static_cast<size_t>(hw) * hh * 3);
    for (int y = 0; y < hh; ++y) {
      for (int x = 0; x < hw; ++x) {
        const size_t idx = (static_cast<size_t>(y) * hw + x) * 3;
        hi[idx + 0] = static_cast<uint8_t>((x * 7 + y * 13) & 0xFF);
        hi[idx + 1] = static_cast<uint8_t>((x * 3 + y * 11) & 0xFF);
        hi[idx + 2] = static_cast<uint8_t>((x * 5 + y * 17) & 0xFF);
      }
    }
    cz::apply_filter_rgb24(hi.data(), hw, hh, cz::FilterId::GameBoy);

    // Check every pixel is strictly one of the 4 DMG colors and every 10x10 block is uniform
    for (int by = 0; by < hh; by += 10) {
      for (int bx = 0; bx < hw; bx += 10) {
        const size_t ref_idx = (static_cast<size_t>(by) * hw + bx) * 3;
        const uint8_t rr = hi[ref_idx + 0];
        const uint8_t rg = hi[ref_idx + 1];
        const uint8_t rb = hi[ref_idx + 2];
        assert(is_dmg_olive(rr, rg, rb));
        for (int dy = 0; dy < 10; ++dy) {
          for (int dx = 0; dx < 10; ++dx) {
            const size_t idx = (static_cast<size_t>(by + dy) * hw + (bx + dx)) * 3;
            assert(hi[idx + 0] == rr);
            assert(hi[idx + 1] == rg);
            assert(hi[idx + 2] == rb);
          }
        }
      }
    }
  }
}

void test_scale_crop_loupe() {
  const int src_w = 640;
  const int src_h = 480;
  std::vector<uint8_t> src(static_cast<size_t>(src_w) * src_h * 3, 0);

  // Outer frame: pure red (255, 0, 0). Central 50%x50% [160..480) x [120..360): pure green (0, 255, 0)
  for (int y = 0; y < src_h; ++y) {
    for (int x = 0; x < src_w; ++x) {
      const size_t idx = (static_cast<size_t>(y) * src_w + x) * 3;
      const bool in_center =
          (x >= src_w / 4 && x < (3 * src_w) / 4 && y >= src_h / 4 &&
           y < (3 * src_h) / 4);
      if (in_center) {
        src[idx + 0] = 0;
        src[idx + 1] = 255;
        src[idx + 2] = 0;
      } else {
        src[idx + 0] = 255;
        src[idx + 1] = 0;
        src[idx + 2] = 0;
      }
    }
  }

  const int dst_w = 226;
  const int dst_h = 170;
  std::vector<uint8_t> dst(static_cast<size_t>(dst_w) * dst_h * 3, 0);

  // With loupe_factor = 1.0f, corners should be red and center should be green
  cz::scale_crop_rgb24(src.data(), src_w, src_h, dst.data(), dst_w, dst_h, 1.0f);
  assert(dst[0] == 255 && dst[1] == 0 && dst[2] == 0);
  const size_t c_idx = (static_cast<size_t>(dst_h / 2) * dst_w + (dst_w / 2)) * 3;
  assert(dst[c_idx] == 0 && dst[c_idx + 1] == 255 && dst[c_idx + 2] == 0);

  // With loupe_factor = 2.0f, crops central 50%x50%, so all 226x170 pixels must be pure green
  std::fill(dst.begin(), dst.end(), 0);
  cz::scale_crop_rgb24(src.data(), src_w, src_h, dst.data(), dst_w, dst_h, 2.0f);
  for (int i = 0; i < dst_w * dst_h; ++i) {
    assert(dst[i * 3 + 0] == 0);
    assert(dst[i * 3 + 1] == 255);
    assert(dst[i * 3 + 2] == 0);
  }
}

void test_jpeg_roundtrip_none_passthrough_and_corrupt_cleanup() {
  TempDir tmp("jpeg");
  std::string err;

  // 1. Encode synthetic 1600x1200 image to .IMG1.jpg.tmp and test FilterId::None passthrough
  const int w = 1600;
  const int h = 1200;
  std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      rgb[idx + 0] = static_cast<uint8_t>((x * 255) / (w - 1));
      rgb[idx + 1] = static_cast<uint8_t>((y * 255) / (h - 1));
      rgb[idx + 2] = 200;
    }
  }

  const std::string tmp1 = tmp.path + "/.IMG1.jpg.tmp";
  const std::string fin1 = tmp.path + "/IMG1.jpg";
  assert(cz::encode_jpeg_rgb24(tmp1, rgb.data(), w, h, 92, err));
  assert(fs::exists(tmp1));
  const std::string raw_tmp1 = read_bytes(tmp1);
  assert(!raw_tmp1.empty());

  assert(cz::finalize_still_photo(tmp1, fin1, cz::FilterId::None, err));
  assert(!fs::exists(tmp1));
  assert(fs::exists(fin1));
  assert(read_bytes(fin1) == raw_tmp1);  // exact byte-for-byte passthrough

  // Full decode check
  cz::RgbImage full;
  assert(cz::decode_jpeg_rgb24(fin1, full, err));
  assert(full.width == w);
  assert(full.height == h);
  assert(full.rgb24.size() == static_cast<size_t>(w) * h * 3);

  // Scaled decode check via libjpeg scale_denom
  cz::RgbImage scaled;
  assert(cz::decode_jpeg_scaled_rgb24(fin1, 226, 170, scaled, err));
  assert(scaled.width > 0 && scaled.height > 0);
  assert(scaled.width < w && scaled.height < h);
  assert(scaled.width >= 226 && scaled.height >= 170);
  assert(scaled.rgb24.size() ==
         static_cast<size_t>(scaled.width) * scaled.height * 3);

  // 2. GameBoy still finalization bakes DMG colors into final_path
  const std::string tmp2 = tmp.path + "/.IMG2.jpg.tmp";
  const std::string fin2 = tmp.path + "/IMG2.jpg";
  assert(cz::encode_jpeg_rgb24(tmp2, rgb.data(), w, h, 92, err));
  assert(cz::finalize_still_photo(tmp2, fin2, cz::FilterId::GameBoy, err));
  assert(!fs::exists(tmp2));
  assert(fs::exists(fin2));

  cz::RgbImage gb_decoded;
  assert(cz::decode_jpeg_rgb24(fin2, gb_decoded, err));
  assert(gb_decoded.width == w && gb_decoded.height == h);
  // Sample points across image to verify DMG olive tones were baked in
  for (int y = 5; y < h; y += 120) {
    for (int x = 5; x < w; x += 160) {
      const size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      assert(is_near_dmg_olive(gb_decoded.rgb24[idx + 0],
                               gb_decoded.rgb24[idx + 1],
                               gb_decoded.rgb24[idx + 2]));
    }
  }

  // 3. Corrupt .tmp cleans up .tmp and leaves no final_path (and no crash/exit)
  const std::string bad_tmp = tmp.path + "/.BAD.jpg.tmp";
  const std::string bad_fin = tmp.path + "/BAD.jpg";
  write_bytes(bad_tmp, "not_a_valid_jpeg_stream_12345");
  assert(!cz::finalize_still_photo(bad_tmp, bad_fin, cz::FilterId::BW, err));
  assert(!err.empty());
  assert(!fs::exists(bad_tmp));
  assert(!fs::exists(bad_fin));
  assert(!fs::exists(bad_tmp + ".filtered.tmp"));

  // Corrupt decode_jpeg_rgb24 returns false cleanly
  cz::RgbImage bad_img;
  const std::string bad_src = tmp.path + "/corrupt.jpg";
  write_bytes(bad_src, "\xff\xd8\xff\xe0garbage");
  assert(!cz::decode_jpeg_rgb24(bad_src, bad_img, err));
  assert(!err.empty());
}

}  // namespace

int main() {
  test_filter_cycle_and_serialization();
  test_all_nine_filters_pixel_behavior();
  test_scale_crop_loupe();
  test_jpeg_roundtrip_none_passthrough_and_corrupt_cleanup();
  std::cout << "test_filters_and_jpeg: all tests passed\n";
  return 0;
}
