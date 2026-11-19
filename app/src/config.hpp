#pragma once

#include <map>
#include <string>

namespace cz {

struct AppConfig {
  std::string output_dir = "~/Pictures/czcamera";
  std::string theme = "dark";
  std::string rail_side = "right";
  std::string photo_resolution = "8mp";
  std::string photo_filter = "none";
  int photo_timer_sec = 0;
  bool grid_enabled = true;
  std::string video_resolution = "720p";
  bool video_audio = true;
  std::string timelapse_resolution = "2mp";
  int timelapse_interval_sec = 5;
  std::string timelapse_output_mode = "video";
  int timelapse_fps = 24;
  std::string timelapse_stop_mode = "manual";
  int timelapse_stop_value = 0;
  int timelapse_dim_sec = 15;
  double ev = 0.0;
  std::string camera_transform = "auto";
};

std::string default_config_path();
std::string expand_user_path(const std::string& path);
AppConfig load_config(const std::string& path = "");
bool save_config(const AppConfig& config, const std::string& path = "");

namespace json_detail {

struct JsonValue {
  enum class Type { Null, Bool, Number, String, Other };
  Type type = Type::Null;
  bool bool_val = false;
  double num_val = 0.0;
  std::string str_val;
};

bool parse_json_object(const std::string& text,
                       std::map<std::string, JsonValue>& out);
std::string escape_json_string(const std::string& input);

}  // namespace json_detail

}  // namespace cz
