#include "jpeg_pipeline.hpp"

#include "storage.hpp"

#include <algorithm>
#include <cerrno>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unistd.h>

#include <jpeglib.h>

namespace fs = std::filesystem;

namespace cz {

namespace {

struct CzJpegErrorMgr {
  jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
  char message[JMSG_LENGTH_MAX];
};

void cz_jpeg_error_exit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<CzJpegErrorMgr*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->message);
  longjmp(err->setjmp_buffer, 1);
}

void cz_jpeg_output_message(j_common_ptr /*cinfo*/) {
  // Suppress libjpeg stderr chatter; errors are surfaced via out_err.
}

void remove_silent(const std::string& path) {
  if (path.empty()) return;
  std::error_code ec;
  fs::remove(path, ec);
}

bool decode_jpeg_impl(const std::string& path,
                      int target_w,
                      int target_h,
                      bool use_scaling,
                      RgbImage& out,
                      std::string& out_err) {
  out_err.clear();
  out = RgbImage{};

  if (path.empty()) {
    out_err = "empty JPEG path";
    return false;
  }

  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    out_err = "failed to open JPEG '" + path + "': " + std::strerror(errno);
    return false;
  }

  jpeg_decompress_struct cinfo{};
  CzJpegErrorMgr jerr{};
  jerr.message[0] = '\0';

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = cz_jpeg_error_exit;
  jerr.pub.output_message = cz_jpeg_output_message;

  if (setjmp(jerr.setjmp_buffer) != 0) {
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    out = RgbImage{};
    out_err = jerr.message[0] != '\0' ? std::string(jerr.message)
                                      : std::string("libjpeg decode error");
    return false;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, fp);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = JCS_RGB;

  if (use_scaling && target_w > 0 && target_h > 0) {
    static constexpr unsigned int kDenoms[] = {8, 4, 2, 1};
    for (unsigned int denom : kDenoms) {
      cinfo.scale_num = 1;
      cinfo.scale_denom = denom;
      jpeg_calc_output_dimensions(&cinfo);
      if (cinfo.output_width >= static_cast<JDIMENSION>(target_w) &&
          cinfo.output_height >= static_cast<JDIMENSION>(target_h)) {
        break;
      }
    }
  }

  jpeg_start_decompress(&cinfo);

  if (cinfo.output_width == 0 || cinfo.output_height == 0 ||
      cinfo.output_components != 3) {
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    out_err = "unexpected JPEG output dimensions or components";
    return false;
  }

  const int w = static_cast<int>(cinfo.output_width);
  const int h = static_cast<int>(cinfo.output_height);
  const size_t row_stride = static_cast<size_t>(w) * 3u;

  out.width = w;
  out.height = h;
  out.rgb24.resize(row_stride * static_cast<size_t>(h));

  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW row_ptr[1];
    row_ptr[0] = out.rgb24.data() +
                 static_cast<size_t>(cinfo.output_scanline) * row_stride;
    jpeg_read_scanlines(&cinfo, row_ptr, 1);
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  std::fclose(fp);
  return true;
}

}  // namespace

bool decode_jpeg_rgb24(const std::string& path,
                       RgbImage& out,
                       std::string& out_err) {
  return decode_jpeg_impl(path, 0, 0, false, out, out_err);
}

bool decode_jpeg_scaled_rgb24(const std::string& path,
                              int target_w,
                              int target_h,
                              RgbImage& out,
                              std::string& out_err) {
  return decode_jpeg_impl(path, target_w, target_h, true, out, out_err);
}

bool encode_jpeg_rgb24(const std::string& path,
                       const uint8_t* rgb,
                       int width,
                       int height,
                       int quality,
                       std::string& out_err) {
  out_err.clear();
  if (path.empty() || !rgb || width <= 0 || height <= 0) {
    out_err = "invalid JPEG encode arguments";
    return false;
  }

  const int clamped_quality = std::max(1, std::min(100, quality));
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    out_err = "failed to open output JPEG '" + path + "': " + std::strerror(errno);
    return false;
  }

  jpeg_compress_struct cinfo{};
  CzJpegErrorMgr jerr{};
  jerr.message[0] = '\0';

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = cz_jpeg_error_exit;
  jerr.pub.output_message = cz_jpeg_output_message;

  if (setjmp(jerr.setjmp_buffer) != 0) {
    jpeg_destroy_compress(&cinfo);
    std::fclose(fp);
    remove_silent(path);
    out_err = jerr.message[0] != '\0' ? std::string(jerr.message)
                                      : std::string("libjpeg encode error");
    return false;
  }

  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, fp);

  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, clamped_quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  const size_t row_stride = static_cast<size_t>(width) * 3u;
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row_ptr[1];
    row_ptr[0] = const_cast<JSAMPROW>(
        rgb + static_cast<size_t>(cinfo.next_scanline) * row_stride);
    jpeg_write_scanlines(&cinfo, row_ptr, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  if (std::fflush(fp) != 0) {
    out_err = std::string("fflush failed: ") + std::strerror(errno);
    std::fclose(fp);
    remove_silent(path);
    return false;
  }
  ::fsync(fileno(fp));
  if (std::fclose(fp) != 0) {
    out_err = std::string("fclose failed: ") + std::strerror(errno);
    remove_silent(path);
    return false;
  }
  return true;
}

bool finalize_still_photo(const std::string& tmp_path,
                          const std::string& final_path,
                          FilterId filter,
                          std::string& out_err) {
  out_err.clear();
  if (tmp_path.empty() || final_path.empty()) {
    out_err = "empty staging path";
    return false;
  }

  if (filter == FilterId::None) {
    if (!commit_staged_file(tmp_path, final_path, out_err)) {
      remove_silent(tmp_path);
      return false;
    }
    return true;
  }

  RgbImage img;
  if (!decode_jpeg_rgb24(tmp_path, img, out_err)) {
    remove_silent(tmp_path);
    return false;
  }

  apply_filter_rgb24(img.rgb24.data(), img.width, img.height, filter);

  const std::string filtered_tmp = tmp_path + ".filtered.tmp";
  if (!encode_jpeg_rgb24(filtered_tmp, img.rgb24.data(), img.width, img.height,
                         92, out_err)) {
    remove_silent(filtered_tmp);
    remove_silent(tmp_path);
    return false;
  }

  if (!commit_staged_file(filtered_tmp, final_path, out_err)) {
    remove_silent(filtered_tmp);
    remove_silent(tmp_path);
    return false;
  }

  if (tmp_path != final_path) {
    remove_silent(tmp_path);
  }
  return true;
}

}  // namespace cz
