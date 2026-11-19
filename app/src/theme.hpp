#pragma once

#include <cstdint>
#include <string>

namespace cz {

enum class ThemeMode {
  Dark,
  Light,
};

struct ShadcnPalette {
  ThemeMode mode = ThemeMode::Dark;
  uint32_t background = 0x09090b;
  uint32_t card = 0x09090b;
  uint32_t muted = 0x18181b;
  uint32_t border = 0x27272a;
  uint32_t ring = 0xd4d4d8;
  uint32_t foreground = 0xfafafa;
  uint32_t muted_foreground = 0xa1a1aa;
  uint32_t active_tab = 0x27272a;
  uint32_t active_tab_foreground = 0xfafafa;
  uint32_t status_online = 0x4ade80;
  uint32_t status_online_bg = 0x11261a;
  uint32_t status_sleeping = 0xfbbf24;
  uint32_t status_sleeping_bg = 0x271d0b;
  uint32_t status_error = 0xf87171;
  uint32_t status_error_bg = 0x2b1214;
  uint32_t viewfinder_surround = 0x050507;
};

ShadcnPalette get_palette(ThemeMode mode);
ThemeMode toggle_theme(ThemeMode mode);
std::string theme_to_string(ThemeMode mode);
ThemeMode theme_from_string(const std::string& value);

}  // namespace cz
