#include "config.hpp"
#include "storage.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unistd.h>

namespace cz {

namespace {

bool is_one_of(const std::string& val, std::initializer_list<const char*> opts) {
  for (const char* opt : opts) {
    if (val == opt) {
      return true;
    }
  }
  return false;
}

AppConfig sanitize_config(AppConfig cfg) {
  if (cfg.output_dir.empty()) {
    cfg.output_dir = "~/Pictures/czcamera";
  }
  if (!is_one_of(cfg.theme, {"dark", "light"})) {
    cfg.theme = "dark";
  }
  if (!is_one_of(cfg.rail_side, {"right", "left"})) {
    cfg.rail_side = "right";
  }
  if (!is_one_of(cfg.photo_resolution, {"8mp", "2mp"})) {
    cfg.photo_resolution = "8mp";
  }
  if (!is_one_of(cfg.photo_filter,
                 {"none", "bw", "hicon", "sepia", "cool", "vivid", "invert",
                  "retro", "gameboy"})) {
    cfg.photo_filter = "none";
  }
  if (cfg.photo_timer_sec != 0 && cfg.photo_timer_sec != 3 &&
      cfg.photo_timer_sec != 10) {
    cfg.photo_timer_sec = 0;
  }
  if (!is_one_of(cfg.video_resolution, {"480p", "720p", "1080p"})) {
    cfg.video_resolution = "720p";
  }
  if (!is_one_of(cfg.timelapse_resolution, {"2mp", "1080p", "8mp"})) {
    cfg.timelapse_resolution = "2mp";
  }
  cfg.timelapse_interval_sec = std::clamp(cfg.timelapse_interval_sec, 1, 3600);
  if (!is_one_of(cfg.timelapse_output_mode, {"video", "photos"})) {
    cfg.timelapse_output_mode = "video";
  }
  cfg.timelapse_fps = std::clamp(cfg.timelapse_fps, 1, 30);
  if (!is_one_of(cfg.timelapse_stop_mode, {"manual", "shots", "duration"})) {
    cfg.timelapse_stop_mode = "manual";
  }
  cfg.timelapse_stop_value = std::max(0, cfg.timelapse_stop_value);
  cfg.timelapse_dim_sec = std::clamp(cfg.timelapse_dim_sec, 0, 3600);
  if (!std::isfinite(cfg.ev)) {
    cfg.ev = 0.0;
  } else {
    cfg.ev = std::clamp(cfg.ev, -2.0, 2.0);
  }
  if (!is_one_of(cfg.camera_transform, {"auto", "0", "180", "hflip", "vflip"})) {
    cfg.camera_transform = "auto";
  }
  return cfg;
}

class JsonParser {
 public:
  explicit JsonParser(const std::string& src) : src_(src), pos_(0) {}

  bool parse_root(std::map<std::string, json_detail::JsonValue>& out) {
    out.clear();
    skip_ws();
    if (!consume('{')) {
      return false;
    }
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      skip_ws();
      return pos_ == src_.size();
    }
    while (true) {
      skip_ws();
      std::string key;
      if (!parse_string(key)) {
        return false;
      }
      skip_ws();
      if (!consume(':')) {
        return false;
      }
      skip_ws();
      json_detail::JsonValue val;
      if (!parse_value(val)) {
        return false;
      }
      out[key] = val;
      skip_ws();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        skip_ws();
        return pos_ == src_.size();
      }
      return false;
    }
  }

 private:
  const std::string& src_;
  size_t pos_;

  char peek() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }

  void skip_ws() {
    while (pos_ < src_.size() &&
           (src_[pos_] == ' ' || src_[pos_] == '\t' || src_[pos_] == '\n' ||
            src_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool consume(char c) {
    if (pos_ < src_.size() && src_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool match_literal(const char* lit, size_t len) {
    if (pos_ + len > src_.size()) {
      return false;
    }
    if (src_.compare(pos_, len, lit) == 0) {
      pos_ += len;
      return true;
    }
    return false;
  }

  static void append_utf8(uint32_t cp, std::string& out) {
    if (cp <= 0x7Fu) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
      out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
  }

  bool parse_hex4(uint32_t& cp) {
    if (pos_ + 4 > src_.size()) {
      return false;
    }
    cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = src_[pos_++];
      cp <<= 4;
      if (c >= '0' && c <= '9') {
        cp |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        cp |= static_cast<uint32_t>(10 + (c - 'a'));
      } else if (c >= 'A' && c <= 'F') {
        cp |= static_cast<uint32_t>(10 + (c - 'A'));
      } else {
        return false;
      }
    }
    return true;
  }

  bool parse_string(std::string& out) {
    out.clear();
    if (!consume('"')) {
      return false;
    }
    while (pos_ < src_.size()) {
      const unsigned char uc = static_cast<unsigned char>(src_[pos_++]);
      if (uc == '"') {
        return true;
      }
      if (uc < 0x20u) {
        return false;
      }
      if (uc == '\\') {
        if (pos_ >= src_.size()) {
          return false;
        }
        const char esc = src_[pos_++];
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!parse_hex4(cp)) {
              return false;
            }
            append_utf8(cp, out);
            break;
          }
          default:
            return false;
        }
      } else {
        out.push_back(static_cast<char>(uc));
      }
    }
    return false;
  }

  bool parse_number(double& out) {
    const size_t start = pos_;
    if (peek() == '-') {
      ++pos_;
    }
    if (peek() == '0') {
      ++pos_;
    } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        ++pos_;
      }
    } else {
      return false;
    }
    if (peek() == '.') {
      ++pos_;
      if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        return false;
      }
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        ++pos_;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') {
        ++pos_;
      }
      if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        return false;
      }
      while (std::isdigit(static_cast<unsigned char>(peek()))) {
        ++pos_;
      }
    }
    const std::string token = src_.substr(start, pos_ - start);
    char* endptr = nullptr;
    out = std::strtod(token.c_str(), &endptr);
    return endptr == token.c_str() + token.size() && std::isfinite(out);
  }

  bool skip_nested_array() {
    if (!consume('[')) {
      return false;
    }
    skip_ws();
    if (consume(']')) {
      return true;
    }
    while (true) {
      skip_ws();
      json_detail::JsonValue unused;
      if (!parse_value(unused)) {
        return false;
      }
      skip_ws();
      if (consume(',')) {
        continue;
      }
      return consume(']');
    }
  }

  bool skip_nested_object() {
    if (!consume('{')) {
      return false;
    }
    skip_ws();
    if (consume('}')) {
      return true;
    }
    while (true) {
      skip_ws();
      std::string k;
      if (!parse_string(k)) {
        return false;
      }
      skip_ws();
      if (!consume(':')) {
        return false;
      }
      skip_ws();
      json_detail::JsonValue unused;
      if (!parse_value(unused)) {
        return false;
      }
      skip_ws();
      if (consume(',')) {
        continue;
      }
      return consume('}');
    }
  }

  bool parse_value(json_detail::JsonValue& out) {
    const char c = peek();
    if (c == '"') {
      out.type = json_detail::JsonValue::Type::String;
      return parse_string(out.str_val);
    }
    if (c == 't' && match_literal("true", 4)) {
      out.type = json_detail::JsonValue::Type::Bool;
      out.bool_val = true;
      return true;
    }
    if (c == 'f' && match_literal("false", 5)) {
      out.type = json_detail::JsonValue::Type::Bool;
      out.bool_val = false;
      return true;
    }
    if (c == 'n' && match_literal("null", 4)) {
      out.type = json_detail::JsonValue::Type::Null;
      return true;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      out.type = json_detail::JsonValue::Type::Number;
      return parse_number(out.num_val);
    }
    if (c == '[') {
      out.type = json_detail::JsonValue::Type::Other;
      return skip_nested_array();
    }
    if (c == '{') {
      out.type = json_detail::JsonValue::Type::Other;
      return skip_nested_object();
    }
    return false;
  }
};

}  // namespace

namespace json_detail {

bool parse_json_object(const std::string& text,
                       std::map<std::string, JsonValue>& out) {
  JsonParser parser(text);
  return parser.parse_root(out);
}

std::string escape_json_string(const std::string& input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (unsigned char c : input) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20u) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

}  // namespace json_detail

std::string expand_user_path(const std::string& path) {
  if (path.empty()) {
    return path;
  }
  if (path == "~" || (path.size() >= 2 && path[0] == '~' && path[1] == '/')) {
    const char* home = std::getenv("HOME");
    if (home && *home) {
      std::string base(home);
      while (base.size() > 1 && base.back() == '/') {
        base.pop_back();
      }
      return path.size() == 1 ? base : base + path.substr(1);
    }
  }
  return path;
}

std::string default_config_path() {
  const char* env_cfg = std::getenv("CZCAMERA_CONFIG");
  if (env_cfg && *env_cfg) {
    return expand_user_path(env_cfg);
  }
  return expand_user_path("~/.config/czcamera/config.json");
}

AppConfig load_config(const std::string& path) {
  const std::string resolved =
      path.empty() ? default_config_path() : expand_user_path(path);
  std::ifstream in(resolved, std::ios::binary);
  if (!in.is_open()) {
    return AppConfig{};
  }
  const std::string contents((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  if (contents.empty()) {
    return AppConfig{};
  }

  std::map<std::string, json_detail::JsonValue> obj;
  if (!json_detail::parse_json_object(contents, obj)) {
    return AppConfig{};
  }

  AppConfig cfg;
  auto get_str = [&](const char* key, std::string& dst) {
    auto it = obj.find(key);
    if (it != obj.end() &&
        it->second.type == json_detail::JsonValue::Type::String) {
      dst = it->second.str_val;
    }
  };
  auto get_int = [&](const char* key, int& dst) {
    auto it = obj.find(key);
    if (it != obj.end() &&
        it->second.type == json_detail::JsonValue::Type::Number) {
      dst = static_cast<int>(std::llround(it->second.num_val));
    }
  };
  auto get_bool = [&](const char* key, bool& dst) {
    auto it = obj.find(key);
    if (it != obj.end() &&
        it->second.type == json_detail::JsonValue::Type::Bool) {
      dst = it->second.bool_val;
    }
  };
  auto get_double = [&](const char* key, double& dst) {
    auto it = obj.find(key);
    if (it != obj.end() &&
        it->second.type == json_detail::JsonValue::Type::Number) {
      dst = it->second.num_val;
    }
  };

  get_str("output_dir", cfg.output_dir);
  get_str("theme", cfg.theme);
  get_str("rail_side", cfg.rail_side);
  get_str("photo_resolution", cfg.photo_resolution);
  get_str("photo_filter", cfg.photo_filter);
  get_int("photo_timer_sec", cfg.photo_timer_sec);
  get_bool("grid_enabled", cfg.grid_enabled);
  get_str("video_resolution", cfg.video_resolution);
  get_bool("video_audio", cfg.video_audio);
  get_str("timelapse_resolution", cfg.timelapse_resolution);
  get_int("timelapse_interval_sec", cfg.timelapse_interval_sec);
  get_str("timelapse_output_mode", cfg.timelapse_output_mode);
  get_int("timelapse_fps", cfg.timelapse_fps);
  get_str("timelapse_stop_mode", cfg.timelapse_stop_mode);
  get_int("timelapse_stop_value", cfg.timelapse_stop_value);
  get_int("timelapse_dim_sec", cfg.timelapse_dim_sec);
  get_double("ev", cfg.ev);
  get_str("camera_transform", cfg.camera_transform);

  return sanitize_config(cfg);
}

bool save_config(const AppConfig& config, const std::string& path) {
  const AppConfig cfg = sanitize_config(config);
  const std::string resolved =
      path.empty() ? default_config_path() : expand_user_path(path);
  if (resolved.empty()) {
    return false;
  }

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(1);
  oss << "{\n"
      << "  \"output_dir\": \""
      << json_detail::escape_json_string(cfg.output_dir) << "\",\n"
      << "  \"theme\": \"" << json_detail::escape_json_string(cfg.theme)
      << "\",\n"
      << "  \"rail_side\": \""
      << json_detail::escape_json_string(cfg.rail_side) << "\",\n"
      << "  \"photo_resolution\": \""
      << json_detail::escape_json_string(cfg.photo_resolution) << "\",\n"
      << "  \"photo_filter\": \""
      << json_detail::escape_json_string(cfg.photo_filter) << "\",\n"
      << "  \"photo_timer_sec\": " << cfg.photo_timer_sec << ",\n"
      << "  \"grid_enabled\": " << (cfg.grid_enabled ? "true" : "false")
      << ",\n"
      << "  \"video_resolution\": \""
      << json_detail::escape_json_string(cfg.video_resolution) << "\",\n"
      << "  \"video_audio\": " << (cfg.video_audio ? "true" : "false") << ",\n"
      << "  \"timelapse_resolution\": \""
      << json_detail::escape_json_string(cfg.timelapse_resolution) << "\",\n"
      << "  \"timelapse_interval_sec\": " << cfg.timelapse_interval_sec << ",\n"
      << "  \"timelapse_output_mode\": \""
      << json_detail::escape_json_string(cfg.timelapse_output_mode) << "\",\n"
      << "  \"timelapse_fps\": " << cfg.timelapse_fps << ",\n"
      << "  \"timelapse_stop_mode\": \""
      << json_detail::escape_json_string(cfg.timelapse_stop_mode) << "\",\n"
      << "  \"timelapse_stop_value\": " << cfg.timelapse_stop_value << ",\n"
      << "  \"timelapse_dim_sec\": " << cfg.timelapse_dim_sec << ",\n"
      << "  \"ev\": " << cfg.ev << ",\n"
      << "  \"camera_transform\": \""
      << json_detail::escape_json_string(cfg.camera_transform) << "\"\n"
      << "}\n";

  const std::string tmp_path =
      resolved + "." + std::to_string(::getpid()) + ".tmp";
  std::string err;
  return write_file_atomic(tmp_path, resolved, oss.str(), err);
}

}  // namespace cz
