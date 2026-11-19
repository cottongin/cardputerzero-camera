#include "gallery_model.hpp"

#include "config.hpp"
#include "video_player.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <jpeglib.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace cz {

namespace {

struct SortableEntry {
  GalleryItem item;
  int64_t mtime_nsec = 0;
};

std::string join_path(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  if (dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

void extract_mtime(const struct stat& st, int64_t& sec, int64_t& nsec) {
#ifdef __APPLE__
  sec = static_cast<int64_t>(st.st_mtimespec.tv_sec);
  nsec = static_cast<int64_t>(st.st_mtimespec.tv_nsec);
#else
  sec = static_cast<int64_t>(st.st_mtim.tv_sec);
  nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
#endif
}

bool is_jpeg_name(const std::string& name) {
  return media_detail::ends_with_ci(name, ".jpg") ||
         media_detail::ends_with_ci(name, ".jpeg");
}

bool is_staging_name(const std::string& name) {
  return name.empty() || name[0] == '.' ||
         media_detail::ends_with_ci(name, ".tmp") ||
         media_detail::ends_with_ci(name, ".part");
}

struct HeaderJpegErr {
  jpeg_error_mgr pub;
  jmp_buf env;
};

void hdr_err_exit(j_common_ptr cinfo) {
  auto* e = reinterpret_cast<HeaderJpegErr*>(cinfo->err);
  longjmp(e->env, 1);
}

void hdr_output_msg(j_common_ptr /*cinfo*/) {}

bool probe_jpeg_dimensions(const std::string& path, int& out_w, int& out_h) {
  out_w = 0;
  out_h = 0;
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;

  jpeg_decompress_struct cinfo{};
  HeaderJpegErr jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = hdr_err_exit;
  jerr.pub.output_message = hdr_output_msg;

  if (setjmp(jerr.env) != 0) {
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    return false;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, fp);
  if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
    out_w = static_cast<int>(cinfo.image_width);
    out_h = static_cast<int>(cinfo.image_height);
  }
  jpeg_destroy_decompress(&cinfo);
  std::fclose(fp);
  return out_w > 0 && out_h > 0;
}

bool decode_scaled_jpeg_preview(const std::string& path,
                                int max_w,
                                int max_h,
                                RgbImage& out,
                                std::string& out_err) {
  RgbImage decoded;
  if (!decode_jpeg_scaled_rgb24(path, max_w, max_h, decoded, out_err)) {
    return false;
  }
  if (decoded.width <= max_w && decoded.height <= max_h) {
    out = std::move(decoded);
    return true;
  }
  if (!media_detail::aspect_scale_rgb24(
          decoded.rgb24.data(), decoded.width, decoded.height, max_w, max_h,
          out.rgb24, out.width, out.height)) {
    out_err = "failed to scale JPEG preview";
    return false;
  }
  return true;
}

bool extract_video_poster_rgb24(const std::string& mp4_path,
                                int target_w,
                                int target_h,
                                RgbImage& out,
                                std::string& out_err) {
  out = RgbImage{};
  out_err.clear();

  const std::string ffmpeg = media_detail::resolve_executable("ffmpeg");
  if (ffmpeg.empty()) {
    out_err = "ffmpeg executable not found";
    return false;
  }
  media_detail::ignore_sigpipe_once();

  std::vector<std::string> args = {
      ffmpeg,
      "-nostdin",
      "-loglevel",
      "error",
      "-i",
      mp4_path,
      "-frames:v",
      "1",
      "-vf",
      "scale=" + std::to_string(target_w) + ":" + std::to_string(target_h),
      "-f",
      "rawvideo",
      "-pix_fmt",
      "rgb24",
      "-",
  };
  pid_t pid = -1;
  int stdout_fd = -1;
  if (!media_detail::spawn_stdout_process(ffmpeg, args, pid, stdout_fd)) {
    out_err = "failed to spawn ffmpeg for poster frame";
    return false;
  }

  const size_t expected =
      static_cast<size_t>(target_w) * static_cast<size_t>(target_h) * 3u;
  std::vector<uint8_t> buf(expected);
  size_t off = 0;
  const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(4);

  while (off < expected && std::chrono::steady_clock::now() < dl) {
    pollfd pfd{stdout_fd, POLLIN, 0};
    const int rc = ::poll(&pfd, 1, 100);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
      bool hit_eof = false;
      while (off < expected) {
        const ssize_t r = ::read(stdout_fd, buf.data() + off, expected - off);
        if (r > 0) {
          off += static_cast<size_t>(r);
          continue;
        }
        if (r == 0) {
          hit_eof = true;
          break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        hit_eof = true;
        break;
      }
      if (hit_eof) break;
    }
  }

  ::close(stdout_fd);

  int status = 0;
  if (!media_detail::reap_pid_timeout(pid, 500, &status)) {
    pid_t tmp = pid;
    media_detail::kill_and_reap(tmp);
    out_err = "ffmpeg poster decode timed out";
    return false;
  }

  if (off != expected) {
    out_err = "failed to decode video poster frame";
    return false;
  }

  out.width = target_w;
  out.height = target_h;
  out.rgb24 = std::move(buf);
  return true;
}

void summarize_timelapse_folder(const std::string& dir_path,
                                int& out_count,
                                uint64_t& out_bytes) {
  out_count = 0;
  out_bytes = 0;
  DIR* d = ::opendir(dir_path.c_str());
  if (!d) return;

  struct dirent* ent = nullptr;
  while ((ent = ::readdir(d)) != nullptr) {
    const std::string name(ent->d_name);
    if (is_staging_name(name) || !is_jpeg_name(name)) {
      continue;
    }
    const std::string full = join_path(dir_path, name);
    struct stat st {};
    if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > 0) {
      ++out_count;
      out_bytes += static_cast<uint64_t>(st.st_size);
    }
  }
  ::closedir(d);
}

std::string find_first_timelapse_frame(const std::string& dir_path) {
  DIR* d = ::opendir(dir_path.c_str());
  if (!d) return "";

  std::vector<std::string> names;
  struct dirent* ent = nullptr;
  while ((ent = ::readdir(d)) != nullptr) {
    const std::string name(ent->d_name);
    if (is_staging_name(name) || !is_jpeg_name(name)) {
      continue;
    }
    const std::string full = join_path(dir_path, name);
    struct stat st {};
    if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
        st.st_size > 0) {
      names.push_back(name);
    }
  }
  ::closedir(d);
  if (names.empty()) return "";
  std::sort(names.begin(), names.end());
  return join_path(dir_path, names.front());
}

}  // namespace

bool GalleryStore::refresh(const std::string& output_dir) {
  const std::string expanded = expand_user_path(output_dir);
  struct stat st {};
  if (expanded.empty() || ::stat(expanded.c_str(), &st) != 0 ||
      !S_ISDIR(st.st_mode)) {
    items_.clear();
    in_session_ = false;
    session_dir_.clear();
    session_name_.clear();
    root_dir_.clear();
    return false;
  }

  if (in_session_ && expanded == root_dir_) {
    struct stat sst {};
    if (::stat(session_dir_.c_str(), &sst) == 0 && S_ISDIR(sst.st_mode)) {
      return scan_session_dir();
    }
    in_session_ = false;
    session_dir_.clear();
    session_name_.clear();
  }

  root_dir_ = expanded;
  in_session_ = false;
  session_dir_.clear();
  session_name_.clear();
  return scan_root_dir();
}

bool GalleryStore::scan_root_dir() {
  items_.clear();
  DIR* d = ::opendir(root_dir_.c_str());
  if (!d) {
    return false;
  }

  std::vector<SortableEntry> found;
  struct dirent* ent = nullptr;
  while ((ent = ::readdir(d)) != nullptr) {
    const std::string name(ent->d_name);
    if (is_staging_name(name)) {
      continue;
    }
    const std::string full = join_path(root_dir_, name);
    struct stat st {};
    if (::stat(full.c_str(), &st) != 0) {
      continue;
    }

    if (S_ISREG(st.st_mode)) {
      if (is_jpeg_name(name)) {
        SortableEntry se;
        se.item.type = GalleryItemType::Photo;
        se.item.abs_path = full;
        se.item.filename = name;
        se.item.size_bytes = static_cast<uint64_t>(std::max<off_t>(0, st.st_size));
        extract_mtime(st, se.item.mtime_epoch, se.mtime_nsec);
        found.push_back(std::move(se));
      } else if (media_detail::ends_with_ci(name, ".mp4")) {
        SortableEntry se;
        se.item.type = GalleryItemType::Video;
        se.item.abs_path = full;
        se.item.filename = name;
        se.item.size_bytes = static_cast<uint64_t>(std::max<off_t>(0, st.st_size));
        extract_mtime(st, se.item.mtime_epoch, se.mtime_nsec);
        found.push_back(std::move(se));
      }
    } else if (S_ISDIR(st.st_mode) && name.rfind("TL_", 0) == 0) {
      int count = 0;
      uint64_t total_bytes = 0;
      summarize_timelapse_folder(full, count, total_bytes);
      SortableEntry se;
      se.item.type = GalleryItemType::TimelapseFolder;
      se.item.abs_path = full;
      se.item.filename = name;
      se.item.photo_count = count;
      se.item.size_bytes = total_bytes;
      extract_mtime(st, se.item.mtime_epoch, se.mtime_nsec);
      found.push_back(std::move(se));
    }
  }
  ::closedir(d);

  std::sort(found.begin(), found.end(),
            [](const SortableEntry& a, const SortableEntry& b) {
              if (a.item.mtime_epoch != b.item.mtime_epoch) {
                return a.item.mtime_epoch > b.item.mtime_epoch;
              }
              if (a.mtime_nsec != b.mtime_nsec) {
                return a.mtime_nsec > b.mtime_nsec;
              }
              return a.item.filename > b.item.filename;
            });

  items_.reserve(found.size());
  for (auto& se : found) {
    items_.push_back(std::move(se.item));
  }
  return true;
}

bool GalleryStore::scan_session_dir() {
  items_.clear();
  DIR* d = ::opendir(session_dir_.c_str());
  if (!d) {
    return false;
  }

  std::vector<GalleryItem> found;
  struct dirent* ent = nullptr;
  while ((ent = ::readdir(d)) != nullptr) {
    const std::string name(ent->d_name);
    if (is_staging_name(name) || !is_jpeg_name(name)) {
      continue;
    }
    const std::string full = join_path(session_dir_, name);
    struct stat st {};
    if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
      continue;
    }

    GalleryItem item;
    item.type = GalleryItemType::Photo;
    item.abs_path = full;
    item.filename = name;
    item.size_bytes = static_cast<uint64_t>(std::max<off_t>(0, st.st_size));
    int64_t nsec_unused = 0;
    extract_mtime(st, item.mtime_epoch, nsec_unused);
    found.push_back(std::move(item));
  }
  ::closedir(d);

  std::sort(found.begin(), found.end(),
            [](const GalleryItem& a, const GalleryItem& b) {
              return a.filename < b.filename;
            });

  items_ = std::move(found);
  return true;
}

bool GalleryStore::enter_session(int index) {
  if (in_session_ || index < 0 || index >= static_cast<int>(items_.size())) {
    return false;
  }
  const GalleryItem& target = items_[static_cast<size_t>(index)];
  if (target.type != GalleryItemType::TimelapseFolder) {
    return false;
  }
  in_session_ = true;
  session_dir_ = target.abs_path;
  session_name_ = target.filename;
  return scan_session_dir();
}

bool GalleryStore::exit_session() {
  if (!in_session_) {
    return false;
  }
  in_session_ = false;
  session_dir_.clear();
  session_name_.clear();
  return scan_root_dir();
}

bool GalleryStore::in_session() const { return in_session_; }

std::string GalleryStore::session_name() const { return session_name_; }

const std::vector<GalleryItem>& GalleryStore::items() const { return items_; }

bool GalleryStore::probe_video_meta(GalleryItem& item) {
  if (item.type != GalleryItemType::Video || item.abs_path.empty()) {
    return false;
  }
  const media_detail::VideoProbeResult probe =
      media_detail::probe_video_file(item.abs_path);
  if (!probe.ok) {
    return false;
  }
  item.duration_sec = probe.duration_sec;
  item.width = probe.width;
  item.height = probe.height;
  return true;
}

bool GalleryStore::load_preview_rgb24(int index,
                                      int max_w,
                                      int max_h,
                                      RgbImage& out,
                                      std::string& out_err) {
  out = RgbImage{};
  out_err.clear();
  if (index < 0 || index >= static_cast<int>(items_.size())) {
    out_err = "gallery index out of range";
    return false;
  }
  if (max_w <= 0 || max_h <= 0) {
    out_err = "invalid preview dimensions";
    return false;
  }

  GalleryItem& item = items_[static_cast<size_t>(index)];

  if (item.type == GalleryItemType::Photo) {
    if (item.width <= 0 || item.height <= 0) {
      int w = 0;
      int h = 0;
      if (probe_jpeg_dimensions(item.abs_path, w, h)) {
        item.width = w;
        item.height = h;
      }
    }
    return decode_scaled_jpeg_preview(item.abs_path, max_w, max_h, out,
                                      out_err);
  }

  if (item.type == GalleryItemType::TimelapseFolder) {
    const std::string first_frame = find_first_timelapse_frame(item.abs_path);
    if (first_frame.empty()) {
      out_err = "timelapse folder has no valid frames";
      return false;
    }
    return decode_scaled_jpeg_preview(first_frame, max_w, max_h, out, out_err);
  }

  // GalleryItemType::Video
  if (item.width <= 0 || item.height <= 0) {
    if (!probe_video_meta(item)) {
      out_err = "failed to probe video metadata";
      return false;
    }
  }

  int target_w = 0;
  int target_h = 0;
  if (!media_detail::compute_fit_dimensions(item.width, item.height, max_w,
                                            max_h, target_w, target_h)) {
    out_err = "invalid video dimensions";
    return false;
  }
  return extract_video_poster_rgb24(item.abs_path, target_w, target_h, out,
                                    out_err);
}

bool GalleryStore::delete_item(int index, std::string& out_err) {
  out_err.clear();
  if (index < 0 || index >= static_cast<int>(items_.size())) {
    out_err = "gallery index out of range";
    return false;
  }
  const GalleryItem target = items_[static_cast<size_t>(index)];
  if (target.abs_path.empty() || target.abs_path == "/" ||
      target.abs_path == root_dir_) {
    out_err = "refusing unsafe delete path";
    return false;
  }

  std::error_code ec;
  if (target.type == GalleryItemType::TimelapseFolder) {
    fs::remove_all(target.abs_path, ec);
  } else {
    fs::remove(target.abs_path, ec);
  }
  if (ec) {
    out_err = "delete failed: " + ec.message();
    return false;
  }

  items_.erase(items_.begin() + index);
  return true;
}

}  // namespace cz
