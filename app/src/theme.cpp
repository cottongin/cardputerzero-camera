#include "theme.hpp"

#include <algorithm>
#include <cctype>

namespace cz {

ShadcnPalette get_palette(ThemeMode mode) {
  if (mode == ThemeMode::Light) {
    ShadcnPalette p;
    p.mode = ThemeMode::Light;
    p.background = 0xfafafa;
    p.card = 0xffffff;
    p.muted = 0xf4f4f5;
    p.border = 0xe4e4e7;
    p.ring = 0x71717a;
    p.foreground = 0x09090b;
    p.muted_foreground = 0x71717a;
    p.active_tab = 0xffffff;
    p.active_tab_foreground = 0x09090b;
    p.status_online = 0x16a34a;
    p.status_online_bg = 0xdcfce7;
    p.status_sleeping = 0xd97706;
    p.status_sleeping_bg = 0xfef3c7;
    p.status_error = 0xdc2626;
    p.status_error_bg = 0xfee2e2;
    p.viewfinder_surround = 0x050507;
    return p;
  }

  ShadcnPalette p;
  p.mode = ThemeMode::Dark;
  p.background = 0x09090b;
  p.card = 0x09090b;
  p.muted = 0x18181b;
  p.border = 0x27272a;
  p.ring = 0xd4d4d8;
  p.foreground = 0xfafafa;
  p.muted_foreground = 0xa1a1aa;
  p.active_tab = 0x27272a;
  p.active_tab_foreground = 0xfafafa;
  p.status_online = 0x4ade80;
  p.status_online_bg = 0x11261a;
  p.status_sleeping = 0xfbbf24;
  p.status_sleeping_bg = 0x271d0b;
  p.status_error = 0xf87171;
  p.status_error_bg = 0x2b1214;
  p.viewfinder_surround = 0x050507;
  return p;
}

ThemeMode toggle_theme(ThemeMode mode) {
  return mode == ThemeMode::Dark ? ThemeMode::Light : ThemeMode::Dark;
}

std::string theme_to_string(ThemeMode mode) {
  return mode == ThemeMode::Light ? "light" : "dark";
}

ThemeMode theme_from_string(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "light") {
    return ThemeMode::Light;
  }
  return ThemeMode::Dark;
}

}  // namespace cz
