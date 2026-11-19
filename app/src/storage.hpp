#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cz {

struct StagedPaths {
  std::string tmp_path;
  std::string final_path;
};

struct TimelapseSessionPaths {
  std::string session_id;
  std::string work_dir;
  std::string photos_dir;
  std::string manifest_path;
  std::string mp4_tmp_path;
  std::string mp4_final_path;
};

struct TimelapseManifest {
  std::string session_id;
  std::string work_dir;
  std::string created_at;
  std::string resolution = "2mp";
  int interval_sec = 5;
  int fps = 24;
  int frame_count = 0;
  std::string status = "recording";
};

bool mkdir_p(const std::string& path);

bool write_file_atomic(const std::string& tmp_path,
                       const std::string& final_path,
                       const std::string& content,
                       std::string& out_err);

StagedPaths next_photo_paths(const std::string& output_dir);
StagedPaths next_video_paths(const std::string& output_dir);
TimelapseSessionPaths create_timelapse_session(const std::string& output_dir,
                                               bool create_work_dir = true);
StagedPaths next_timelapse_frame_paths(const std::string& dir, int frame_index);

bool commit_staged_file(const std::string& tmp_path,
                        const std::string& final_path,
                        std::string& out_err);

bool has_sufficient_storage(const std::string& dir_path,
                            uint64_t min_bytes = 104857600ULL,
                            uint64_t* out_free_bytes = nullptr);

bool write_timelapse_manifest(const std::string& path_or_dir,
                              const TimelapseManifest& manifest);
bool read_timelapse_manifest(const std::string& path_or_dir,
                             TimelapseManifest& out_manifest);
std::vector<TimelapseManifest> scan_orphaned_timelapses(
    const std::string& output_dir);

}  // namespace cz
