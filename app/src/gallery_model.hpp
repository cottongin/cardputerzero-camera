#pragma once

#include "jpeg_pipeline.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cz {

enum class GalleryItemType {
  Photo,
  Video,
  TimelapseFolder,
};

struct GalleryItem {
  GalleryItemType type = GalleryItemType::Photo;
  std::string abs_path;
  std::string filename;
  int64_t mtime_epoch = 0;
  uint64_t size_bytes = 0;
  int photo_count = 0;
  double duration_sec = 0.0;
  int width = 0;
  int height = 0;
};

class GalleryStore {
 public:
  bool refresh(const std::string& output_dir);
  bool enter_session(int index);
  bool exit_session();

  bool in_session() const;
  std::string session_name() const;
  const std::vector<GalleryItem>& items() const;

  bool load_preview_rgb24(int index,
                          int max_w,
                          int max_h,
                          RgbImage& out,
                          std::string& out_err);
  bool probe_video_meta(GalleryItem& item);
  bool delete_item(int index, std::string& out_err);

 private:
  bool scan_root_dir();
  bool scan_session_dir();

  std::string root_dir_;
  std::string session_dir_;
  std::string session_name_;
  bool in_session_ = false;
  std::vector<GalleryItem> items_;
};

}  // namespace cz
