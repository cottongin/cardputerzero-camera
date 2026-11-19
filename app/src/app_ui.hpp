#pragma once

#include "camera_backend.hpp"
#include "theme.hpp"
#include "ui_controller.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace cz {

/// 320x170 shadcn/ui Zinc Option A Split View & Modal Renderer.
/// Supports optional FreeType2 / LVGL hooks and built-in anti-aliased
/// software framebuffer + 5x7 bitmap fallback.
class AppView {
 public:
  static constexpr int kWidth = 320;
  static constexpr int kHeight = 170;

  AppView();
  ~AppView();

  AppView(const AppView&) = delete;
  AppView& operator=(const AppView&) = delete;

  /// Swap active Zinc Dark / Zinc Light palette tokens without recreating view objects.
  void apply_theme(ThemeMode mode);

  /// Re-render the 320x170 framebuffer from UiState and optional preview frames.
  void render(const UiState& state,
              const PreviewFrame* live_frame = nullptr,
              const PreviewFrame* gallery_frame = nullptr);

  /// Return 320*170 ARGB8888 (0xAARRGGBB) framebuffer pixels.
  const uint32_t* pixels() const;

  /// Write current 320x170 framebuffer as a binary PPM (P6) image.
  bool save_ppm(const std::string& path) const;

  /// True if at least one FreeType font (.ttf/.ttc/.otf) was loaded.
  bool using_freetype() const;

  /// Return currently applied theme mode.
  ThemeMode active_theme() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cz
