#include "app_ui.hpp"
#include "camera_backend.hpp"
#include "evdev_repeat_guard.hpp"
#include "fb_blit.hpp"
#include "storage.hpp"
#include "ui_controller.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(CZ_TARGET_DEVICE) && defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#else
#include <SDL.h>
#endif

namespace fs = std::filesystem;

namespace {

volatile std::sig_atomic_t g_signal_stop = 0;

void handle_signal(int /*sig*/) { g_signal_stop = 1; }

uint64_t steady_now_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void print_usage(const char* argv0) {
  std::cout
      << "Usage: " << argv0 << " [options]\n"
      << "  --sim                      Run desktop 320x170 SDL2 simulator\n"
      << "  --scale <1..4>             Integer window scale factor (default: 2)\n"
      << "  --mock-cam                 Force procedural SimCameraBackend\n"
      << "  --output-dir <path>        Override media output directory\n"
      << "  --render-test <path.ppm>   Render 320x170 snapshot to PPM and exit\n"
      << "  --theme <dark|light>       Theme override (dark or light)\n"
      << "  --mode <photo|video|timelapse|gallery|settings>\n"
      << "                             Initial mode or modal\n"
      << "  --rail <right|left>        Control rail side (right or left)\n"
      << "  --help                     Show this help message\n";
}

struct ScopedTempDir {
  std::string path;
  ~ScopedTempDir() {
    if (!path.empty()) {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  }
};

struct CliOptions {
  int scale = 2;
  bool mock_cam = false;
  std::string output_dir;
  std::string render_test_path;
  std::string theme_override;
  std::string mode_override;
  std::string rail_override;
};

struct RenderDirtyTracker {
  uint64_t gal_seq = 0;
  double vp_pos = -1.0;
  bool vp_playing = false;
  bool flash = false;
  bool dimmed = false;
  bool backend_ready = false;
  bool backend_paused = false;
  bool backend_connecting = false;
  int backend_connect_attempt = 0;
  bool tl_running = false;
  int tl_shots = 0;
  int tl_next = 0;
  int tl_elapsed = 0;
  int vid_sec = 0;
  int timer_sec = 0;
  int comp_cur = 0;
  std::string backend_error;
  std::string toast;
  cz::OverlayModal modal = cz::OverlayModal::None;

  void sync(const cz::UiState& st, uint64_t cur_gal_seq) {
    gal_seq = cur_gal_seq;
    vp_pos = st.video_player.position_sec;
    vp_playing = st.video_player.playing;
    flash = st.capture_flash_active;
    dimmed = st.timelapse_dimmed;
    backend_ready = st.backend_ready;
    backend_paused = st.backend_paused;
    backend_connecting = st.backend_connecting;
    backend_connect_attempt = st.backend_connect_attempt;
    tl_running = st.timelapse.running;
    tl_shots = st.timelapse.shots_taken;
    tl_next = st.timelapse.next_shot_in_sec;
    tl_elapsed = st.timelapse.elapsed_sec;
    vid_sec = st.video_elapsed_sec;
    timer_sec = st.timer_remaining_sec;
    comp_cur = st.timelapse.compile_current;
    backend_error = st.backend_error;
    toast = st.toast_message;
    modal = st.modal;
  }

  bool changed(const cz::UiState& st, uint64_t cur_gal_seq) const {
    return cur_gal_seq != gal_seq ||
           st.video_player.position_sec != vp_pos ||
           st.video_player.playing != vp_playing ||
           st.capture_flash_active != flash ||
           st.timelapse_dimmed != dimmed ||
           st.backend_ready != backend_ready ||
           st.backend_paused != backend_paused ||
           st.backend_connecting != backend_connecting ||
           st.backend_connect_attempt != backend_connect_attempt ||
           st.timelapse.running != tl_running ||
           st.timelapse.shots_taken != tl_shots ||
           st.timelapse.next_shot_in_sec != tl_next ||
           st.timelapse.elapsed_sec != tl_elapsed ||
           st.video_elapsed_sec != vid_sec ||
           st.timer_remaining_sec != timer_sec ||
           st.timelapse.compile_current != comp_cur ||
           st.backend_error != backend_error ||
           st.toast_message != toast ||
           st.modal != modal;
  }
};

void poll_live_frame_state(cz::UiController& ctrl,
                           uint64_t& last_live_seq,
                           cz::PreviewFrame& live_frame,
                           bool& dirty) {
  const auto& st = ctrl.state();
  if (!st.backend_ready || st.backend_paused) {
    if (!live_frame.rgb24.empty()) {
      live_frame = cz::PreviewFrame{};
      dirty = true;
    }
    return;
  }
  if (ctrl.backend() && st.modal != cz::OverlayModal::Gallery) {
    if (ctrl.backend()->copy_latest_preview(last_live_seq, live_frame)) {
      last_live_seq = live_frame.seq;
      dirty = true;
    }
  }
}

const cz::PreviewFrame* effective_live_frame(
    const cz::UiState& st,
    const cz::PreviewFrame& live_frame) {
  return (st.backend_ready && !st.backend_paused) ? &live_frame : nullptr;
}

int run_render_test(const CliOptions& cli) {
  ScopedTempDir temp_dir;
  std::string eff_out = cli.output_dir;
  if (eff_out.empty()) {
    temp_dir.path = "/tmp/czcamera_render_" + std::to_string(::getpid());
    std::error_code ec;
    fs::remove_all(temp_dir.path, ec);
    if (!cz::mkdir_p(temp_dir.path)) {
      std::cerr << "Failed to prepare temporary render directory\n";
      return 1;
    }
    eff_out = temp_dir.path;
  }

  cz::UiControllerOptions opts;
  opts.output_dir_override = eff_out;
  opts.mock_backend = true;
  opts.persist_config = false;
  opts.skip_startup_recovery_scan = true;

  cz::UiController ctrl(nullptr, opts);
  uint64_t now = 1000;
  if (!ctrl.init(now)) {
    std::cerr << "Failed to initialize controller for render-test\n";
    return 1;
  }

  if (!cli.theme_override.empty()) {
    const cz::ThemeMode target_theme = cz::theme_from_string(cli.theme_override);
    if (ctrl.state().theme != target_theme) {
      ctrl.handle_key('t', cz::KeyCode::Char, now);
    }
  }

  if (!cli.rail_override.empty()) {
    const std::string target_rail =
        (cli.rail_override == "left") ? "left" : "right";
    if (ctrl.state().config.rail_side != target_rail) {
      ctrl.handle_key('w', cz::KeyCode::Char, now);
    }
  }

  if (cli.mode_override == "video") {
    ctrl.handle_key('2', cz::KeyCode::Char, now);
  } else if (cli.mode_override == "timelapse") {
    ctrl.handle_key('3', cz::KeyCode::Char, now);
  } else if (cli.mode_override == "gallery") {
    // Capture a realistic sample photo if the target directory has no media yet
    cz::GalleryStore probe;
    probe.refresh(ctrl.state().effective_output_dir);
    if (probe.items().empty()) {
      ctrl.handle_key(0, cz::KeyCode::Enter, now);
      (void)ctrl.consume_capture_flash();
      now += 4000;
      ctrl.tick(now);
    }
    ctrl.handle_key('8', cz::KeyCode::Char, now);
  } else if (cli.mode_override == "settings") {
    ctrl.handle_key('s', cz::KeyCode::Char, now);
  }

  cz::PreviewFrame live_frame;
  if (ctrl.backend()) {
    ctrl.backend()->copy_latest_preview(0, live_frame);
  }

  cz::AppView view;
  view.apply_theme(ctrl.state().theme);
  view.render(ctrl.state(), effective_live_frame(ctrl.state(), live_frame),
              ctrl.gallery_preview());
  const bool ok = view.save_ppm(cli.render_test_path);

  ctrl.shutdown();
  if (!ok) {
    std::cerr << "Failed to write PPM to " << cli.render_test_path << "\n";
    return 1;
  }
  return 0;
}

#if defined(CZ_TARGET_DEVICE) && defined(__linux__)

char map_evdev_code(uint16_t code, bool shift) {
  switch (code) {
    case KEY_1: return shift ? '!' : '1';
    case KEY_2: return shift ? '@' : '2';
    case KEY_3: return shift ? '#' : '3';
    case KEY_4: return shift ? '$' : '4';
    case KEY_5: return shift ? '%' : '5';
    case KEY_6: return shift ? '^' : '6';
    case KEY_7: return shift ? '&' : '7';
    case KEY_8: return shift ? '*' : '8';
    case KEY_9: return shift ? '(' : '9';
    case KEY_0: return shift ? ')' : '0';
    case KEY_A: return shift ? 'A' : 'a';
    case KEY_B: return shift ? 'B' : 'b';
    case KEY_C: return shift ? 'C' : 'c';
    case KEY_D: return shift ? 'D' : 'd';
    case KEY_E: return shift ? 'E' : 'e';
    case KEY_F: return shift ? 'F' : 'f';
    case KEY_G: return shift ? 'G' : 'g';
    case KEY_H: return shift ? 'H' : 'h';
    case KEY_I: return shift ? 'I' : 'i';
    case KEY_J: return shift ? 'J' : 'j';
    case KEY_K: return shift ? 'K' : 'k';
    case KEY_L: return shift ? 'L' : 'l';
    case KEY_M: return shift ? 'M' : 'm';
    case KEY_N: return shift ? 'N' : 'n';
    case KEY_O: return shift ? 'O' : 'o';
    case KEY_P: return shift ? 'P' : 'p';
    case KEY_Q: return shift ? 'Q' : 'q';
    case KEY_R: return shift ? 'R' : 'r';
    case KEY_S: return shift ? 'S' : 's';
    case KEY_T: return shift ? 'T' : 't';
    case KEY_U: return shift ? 'U' : 'u';
    case KEY_V: return shift ? 'V' : 'v';
    case KEY_W: return shift ? 'W' : 'w';
    case KEY_X: return shift ? 'X' : 'x';
    case KEY_Y: return shift ? 'Y' : 'y';
    case KEY_Z: return shift ? 'Z' : 'z';
    case KEY_MINUS: return shift ? '_' : '-';
    case KEY_EQUAL: return shift ? '+' : '=';
    case KEY_LEFTBRACE: return shift ? '{' : '[';
    case KEY_RIGHTBRACE: return shift ? '}' : ']';
    case KEY_SEMICOLON: return shift ? ':' : ';';
    case KEY_APOSTROPHE: return shift ? '"' : '\'';
    case KEY_COMMA: return shift ? '<' : ',';
    case KEY_DOT: return shift ? '>' : '.';
    case KEY_SLASH: return shift ? '?' : '/';
    case KEY_BACKSLASH: return shift ? '|' : '\\';
    case KEY_GRAVE: return shift ? '~' : '`';
    default: return 0;
  }
}

void blit_fbdev(uint8_t* fb_mem,
                size_t map_size,
                const fb_var_screeninfo& vinfo,
                const fb_fix_screeninfo& finfo,
                const uint32_t* pixels) {
  cz::FbBlitSpec spec;
  spec.xres = vinfo.xres;
  spec.yres = vinfo.yres;
  spec.xres_virtual = vinfo.xres_virtual;
  spec.yres_virtual = vinfo.yres_virtual;
  spec.xoffset = vinfo.xoffset;
  spec.yoffset = vinfo.yoffset;
  spec.bits_per_pixel = vinfo.bits_per_pixel;
  spec.line_length = finfo.line_length;
  spec.red = {vinfo.red.offset, vinfo.red.length};
  spec.green = {vinfo.green.offset, vinfo.green.length};
  spec.blue = {vinfo.blue.offset, vinfo.blue.length};
  spec.transp = {vinfo.transp.offset, vinfo.transp.length};
  cz::blit_framebuffer(fb_mem, map_size, spec, pixels, cz::AppView::kWidth,
                       cz::AppView::kHeight);
}

std::vector<int> open_keyboard_evdevs() {
  std::vector<int> fds;
  if (const char* override_dev = std::getenv("CZCAMERA_EVDEV_DEVICE")) {
    const int fd = ::open(override_dev, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
      fds.push_back(fd);
    }
    return fds;
  }

  DIR* dir = ::opendir("/dev/input");
  if (dir == nullptr) {
    return fds;
  }
  struct dirent* ent = nullptr;
  while ((ent = ::readdir(dir)) != nullptr) {
    if (std::strncmp(ent->d_name, "event", 5) != 0) {
      continue;
    }
    const std::string dev_path = std::string("/dev/input/") + ent->d_name;
    const int fd = ::open(dev_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }
    unsigned long ev_bits[4]{};
    if (::ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) >= 0 &&
        (ev_bits[0] & (1UL << EV_KEY)) != 0) {
      fds.push_back(fd);
    } else {
      ::close(fd);
    }
  }
  ::closedir(dir);
  return fds;
}

int run_device_loop(cz::UiController& ctrl, cz::AppView& view) {
  const char* fb_dev = std::getenv("CZCAMERA_FB_DEVICE");
  if (fb_dev == nullptr || fb_dev[0] == '\0') {
    fb_dev = "/dev/fb0";
  }
  const int fb_fd = ::open(fb_dev, O_RDWR);
  if (fb_fd < 0) {
    std::cerr << "Failed to open framebuffer " << fb_dev << "\n";
    return 1;
  }

  fb_var_screeninfo vinfo{};
  fb_fix_screeninfo finfo{};
  if (::ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
      ::ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
    std::cerr << "Failed to query framebuffer info\n";
    ::close(fb_fd);
    return 1;
  }

  const size_t map_size = finfo.smem_len;
  if (map_size == 0) {
    std::cerr << "Framebuffer reports zero memory length\n";
    ::close(fb_fd);
    return 1;
  }
  void* mapped = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        fb_fd, 0);
  if (mapped == MAP_FAILED) {
    std::cerr << "Failed to mmap framebuffer\n";
    ::close(fb_fd);
    return 1;
  }
  auto* fb_mem = static_cast<uint8_t*>(mapped);

  std::vector<int> ev_fds = open_keyboard_evdevs();
  std::vector<pollfd> pfds;
  pfds.reserve(ev_fds.size());
  for (int fd : ev_fds) {
    pfds.push_back(pollfd{fd, POLLIN, 0});
  }

  cz::PreviewFrame live_frame;
  uint64_t last_live_seq = 0;

  view.apply_theme(ctrl.state().theme);
  if (ctrl.backend() && ctrl.state().backend_ready &&
      !ctrl.state().backend_paused) {
    if (ctrl.backend()->copy_latest_preview(last_live_seq, live_frame)) {
      last_live_seq = live_frame.seq;
    }
  }
  const cz::PreviewFrame* init_gp = ctrl.gallery_preview();
  view.render(ctrl.state(), effective_live_frame(ctrl.state(), live_frame),
              init_gp);
  blit_fbdev(fb_mem, map_size, vinfo, finfo, view.pixels());

  bool shift_down = false;
  cz::EvdevRepeatGuard repeat_guard;
  RenderDirtyTracker dirty_tracker;
  dirty_tracker.sync(ctrl.state(), init_gp ? init_gp->seq : 0);

  while (!ctrl.should_quit() && !g_signal_stop) {
    bool dirty = false;
    if (!pfds.empty()) {
      const int prc = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 16);
      if (prc < 0) {
        if (errno != EINTR) {
          ::usleep(16000);
        }
      } else if (prc > 0) {
        std::vector<size_t> dead_indices;
        for (size_t idx = 0;
             idx < pfds.size() && !ctrl.should_quit() && !g_signal_stop; ++idx) {
          const auto& p = pfds[idx];
          bool remove_fd = (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
          if ((p.revents & POLLIN) != 0) {
            while (!ctrl.should_quit() && !g_signal_stop) {
              struct input_event ev {};
              const ssize_t n = ::read(p.fd, &ev, sizeof(ev));
              if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                remove_fd = true;
                break;
              }
              if (n == 0) {
                remove_fd = true;
                break;
              }
              if (n != static_cast<ssize_t>(sizeof(ev))) {
                break;
              }
              if (ev.type != EV_KEY) {
                continue;
              }
              if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
                shift_down = (ev.value != 0);
                continue;
              }
              if (!repeat_guard.should_dispatch(
                      ev.code, ev.value, ctrl.state().timelapse_dimmed)) {
                continue;
              }
              const uint64_t now = steady_now_ms();
              bool handled = false;
              switch (ev.code) {
                case KEY_UP:
                  handled = ctrl.handle_key(0, cz::KeyCode::Up, now);
                  break;
                case KEY_DOWN:
                  handled = ctrl.handle_key(0, cz::KeyCode::Down, now);
                  break;
                case KEY_LEFT:
                  handled = ctrl.handle_key(0, cz::KeyCode::Left, now);
                  break;
                case KEY_RIGHT:
                  handled = ctrl.handle_key(0, cz::KeyCode::Right, now);
                  break;
                case KEY_ENTER:
                case KEY_KPENTER:
                  handled = ctrl.handle_key(0, cz::KeyCode::Enter, now);
                  break;
                case KEY_ESC:
                  handled = ctrl.handle_key(0, cz::KeyCode::Esc, now);
                  break;
                case KEY_BACKSPACE:
                  handled = ctrl.handle_key(0, cz::KeyCode::Backspace, now);
                  break;
                case KEY_DELETE:
                  handled = ctrl.handle_key(0, cz::KeyCode::Delete, now);
                  break;
                case KEY_SPACE:
                  handled = ctrl.handle_key(' ', cz::KeyCode::Space, now);
                  break;
                default: {
                  const char c = map_evdev_code(ev.code, shift_down);
                  if (c != 0) {
                    handled = ctrl.handle_key(c, cz::KeyCode::Char, now);
                  }
                  break;
                }
              }
              if (handled) {
                dirty = true;
              }
            }
          }
          if (remove_fd) {
            ::close(p.fd);
            dead_indices.push_back(idx);
          }
        }
        for (auto it = dead_indices.rbegin(); it != dead_indices.rend(); ++it) {
          pfds.erase(pfds.begin() + static_cast<std::ptrdiff_t>(*it));
        }
      }
    } else {
      ::usleep(16000);
    }

    if (ctrl.should_quit() || g_signal_stop) {
      break;
    }

    const uint64_t now = steady_now_ms();
    ctrl.tick(now);

    poll_live_frame_state(ctrl, last_live_seq, live_frame, dirty);

    const cz::PreviewFrame* gp = ctrl.gallery_preview();
    const uint64_t gal_seq = gp ? gp->seq : 0;
    const auto& st = ctrl.state();
    if (dirty_tracker.changed(st, gal_seq)) {
      dirty = true;
    }

    if (dirty) {
      dirty_tracker.sync(st, gal_seq);
      view.render(st, effective_live_frame(st, live_frame), gp);
      blit_fbdev(fb_mem, map_size, vinfo, finfo, view.pixels());
    }
  }

  for (const auto& p : pfds) {
    ::close(p.fd);
  }
  ::munmap(mapped, map_size);
  ::close(fb_fd);
  return 0;
}

#else

int run_sdl_loop(cz::UiController& ctrl, cz::AppView& view, int scale) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
    std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
    return 1;
  }

  SDL_AudioDeviceID audio_dev = 0;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
    SDL_AudioSpec want{};
    SDL_AudioSpec have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    audio_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio_dev != 0) {
      SDL_PauseAudioDevice(audio_dev, 0);
      ctrl.video_player().set_pcm_sink(
          [audio_dev](const int16_t* samples, size_t count) {
            if (samples != nullptr && count > 0) {
              SDL_QueueAudio(audio_dev, samples,
                             static_cast<Uint32>(count * sizeof(int16_t)));
            }
          });
    }
  }

  SDL_Window* window = SDL_CreateWindow(
      "czcamera", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      cz::AppView::kWidth * scale, cz::AppView::kHeight * scale,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (window == nullptr) {
    std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
    ctrl.video_player().set_pcm_sink(nullptr);
    if (audio_dev != 0) SDL_CloseAudioDevice(audio_dev);
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer == nullptr) {
    renderer = SDL_CreateRenderer(window, -1, 0);
  }
  if (renderer == nullptr) {
    std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
    SDL_DestroyWindow(window);
    ctrl.video_player().set_pcm_sink(nullptr);
    if (audio_dev != 0) SDL_CloseAudioDevice(audio_dev);
    SDL_Quit();
    return 1;
  }

  SDL_RenderSetLogicalSize(renderer, cz::AppView::kWidth,
                           cz::AppView::kHeight);
  SDL_Texture* texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      cz::AppView::kWidth, cz::AppView::kHeight);
  if (texture == nullptr) {
    std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << "\n";
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    ctrl.video_player().set_pcm_sink(nullptr);
    if (audio_dev != 0) SDL_CloseAudioDevice(audio_dev);
    SDL_Quit();
    return 1;
  }

  SDL_StartTextInput();

  cz::PreviewFrame live_frame;
  uint64_t last_live_seq = 0;

  view.apply_theme(ctrl.state().theme);
  if (ctrl.backend() && ctrl.state().backend_ready &&
      !ctrl.state().backend_paused) {
    if (ctrl.backend()->copy_latest_preview(last_live_seq, live_frame)) {
      last_live_seq = live_frame.seq;
    }
  }
  const cz::PreviewFrame* init_gp = ctrl.gallery_preview();
  view.render(ctrl.state(), effective_live_frame(ctrl.state(), live_frame),
              init_gp);

  auto present = [&]() {
    SDL_UpdateTexture(texture, nullptr, view.pixels(),
                      cz::AppView::kWidth * static_cast<int>(sizeof(uint32_t)));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
  };
  present();

  bool running = true;
  RenderDirtyTracker dirty_tracker;
  dirty_tracker.sync(ctrl.state(), init_gp ? init_gp->seq : 0);

  while (running && !ctrl.should_quit() && !g_signal_stop) {
    bool dirty = false;
    SDL_Event ev;
    if (SDL_WaitEventTimeout(&ev, 16) != 0) {
      do {
        if (ev.type == SDL_QUIT) {
          running = false;
          break;
        }
        if (ev.type == SDL_WINDOWEVENT &&
            (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
             ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
          dirty = true;
          continue;
        }
        if (ev.type == SDL_TEXTINPUT) {
          const uint64_t now = steady_now_ms();
          for (int i = 0;
               running && !ctrl.should_quit() && !g_signal_stop &&
               ev.text.text[i] != '\0';
               ++i) {
            if (ctrl.handle_key(ev.text.text[i], cz::KeyCode::Char, now)) {
              dirty = true;
            }
          }
          continue;
        }
        if (ev.type == SDL_KEYDOWN) {
          const SDL_Keycode sym = ev.key.keysym.sym;
          const uint16_t mod = ev.key.keysym.mod;
          if ((mod & (KMOD_CTRL | KMOD_GUI)) != 0 && sym == SDLK_q) {
            running = false;
            break;
          }
          const uint64_t now = steady_now_ms();
          bool handled = false;
          switch (sym) {
            case SDLK_UP:
              handled = ctrl.handle_key(0, cz::KeyCode::Up, now);
              break;
            case SDLK_DOWN:
              handled = ctrl.handle_key(0, cz::KeyCode::Down, now);
              break;
            case SDLK_LEFT:
              handled = ctrl.handle_key(0, cz::KeyCode::Left, now);
              break;
            case SDLK_RIGHT:
              handled = ctrl.handle_key(0, cz::KeyCode::Right, now);
              break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
              handled = ctrl.handle_key(0, cz::KeyCode::Enter, now);
              break;
            case SDLK_ESCAPE:
              handled = ctrl.handle_key(0, cz::KeyCode::Esc, now);
              break;
            case SDLK_BACKSPACE:
              handled = ctrl.handle_key(0, cz::KeyCode::Backspace, now);
              break;
            case SDLK_DELETE:
              handled = ctrl.handle_key(0, cz::KeyCode::Delete, now);
              break;
            default:
              break;
          }
          if (handled) {
            dirty = true;
          }
        }
      } while (running && !ctrl.should_quit() && SDL_PollEvent(&ev) != 0);
    }

    if (!running || ctrl.should_quit() || g_signal_stop) {
      break;
    }

    const uint64_t now = steady_now_ms();
    ctrl.tick(now);

    poll_live_frame_state(ctrl, last_live_seq, live_frame, dirty);

    const auto& st = ctrl.state();
    if (audio_dev != 0) {
      if ((!st.video_player.playing && dirty_tracker.vp_playing) ||
          (st.video_player.playing &&
           std::abs(st.video_player.position_sec - dirty_tracker.vp_pos) >
               0.45)) {
        SDL_ClearQueuedAudio(audio_dev);
      }
    }

    const cz::PreviewFrame* gp = ctrl.gallery_preview();
    const uint64_t gal_seq = gp ? gp->seq : 0;
    if (dirty_tracker.changed(st, gal_seq)) {
      dirty = true;
    }

    if (dirty) {
      dirty_tracker.sync(st, gal_seq);
      view.render(st, effective_live_frame(st, live_frame), gp);
      present();
    }
  }

  ctrl.video_player().set_pcm_sink(nullptr);
  ctrl.shutdown();
  if (audio_dev != 0) {
    SDL_CloseAudioDevice(audio_dev);
  }
  SDL_StopTextInput();
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

#endif

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  CliOptions cli;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    }
    if (arg == "--sim") {
      continue;
    }
    if (arg == "--mock-cam") {
      cli.mock_cam = true;
      continue;
    }
    if (arg == "--scale" && i + 1 < argc) {
      const int parsed = std::atoi(argv[++i]);
      if (parsed < 1 || parsed > 4) {
        std::cerr << "Invalid --scale value (expected 1..4)\n";
        return 2;
      }
      cli.scale = parsed;
      continue;
    }
    if (arg == "--output-dir" && i + 1 < argc) {
      cli.output_dir = argv[++i];
      continue;
    }
    if (arg == "--render-test" && i + 1 < argc) {
      cli.render_test_path = argv[++i];
      continue;
    }
    if (arg == "--theme" && i + 1 < argc) {
      cli.theme_override = argv[++i];
      if (cli.theme_override != "dark" && cli.theme_override != "light") {
        std::cerr << "Invalid --theme value: " << cli.theme_override << "\n";
        return 2;
      }
      continue;
    }
    if (arg == "--mode" && i + 1 < argc) {
      cli.mode_override = argv[++i];
      if (cli.mode_override != "photo" && cli.mode_override != "video" &&
          cli.mode_override != "timelapse" && cli.mode_override != "gallery" &&
          cli.mode_override != "settings") {
        std::cerr << "Invalid --mode value: " << cli.mode_override << "\n";
        return 2;
      }
      continue;
    }
    if (arg == "--rail" && i + 1 < argc) {
      cli.rail_override = argv[++i];
      if (cli.rail_override != "right" && cli.rail_override != "left") {
        std::cerr << "Invalid --rail value: " << cli.rail_override << "\n";
        return 2;
      }
      continue;
    }
    std::cerr << "Unknown or incomplete option: " << arg << "\n";
    print_usage(argv[0]);
    return 2;
  }

  if (!cli.render_test_path.empty()) {
    return run_render_test(cli);
  }

  cz::UiControllerOptions opts;
  opts.output_dir_override = cli.output_dir;
  opts.mock_backend = cli.mock_cam;
  opts.async_backend_start = !cli.mock_cam;
  opts.defer_initial_backend_start = (cli.mode_override == "gallery");

  cz::UiController ctrl(nullptr, opts);
  const uint64_t now = steady_now_ms();
  ctrl.init(now);

  if (!cli.theme_override.empty()) {
    const cz::ThemeMode target_theme = cz::theme_from_string(cli.theme_override);
    if (ctrl.state().theme != target_theme) {
      ctrl.handle_key('t', cz::KeyCode::Char, now);
    }
  }
  if (!cli.rail_override.empty() &&
      ctrl.state().config.rail_side != cli.rail_override) {
    ctrl.handle_key('w', cz::KeyCode::Char, now);
  }
  if (cli.mode_override == "video") {
    ctrl.handle_key('2', cz::KeyCode::Char, now);
  } else if (cli.mode_override == "timelapse") {
    ctrl.handle_key('3', cz::KeyCode::Char, now);
  } else if (cli.mode_override == "gallery") {
    ctrl.handle_key('8', cz::KeyCode::Char, now);
  } else if (cli.mode_override == "settings") {
    ctrl.handle_key('s', cz::KeyCode::Char, now);
  }

  cz::AppView view;
#if defined(CZ_TARGET_DEVICE) && defined(__linux__)
  (void)cli.scale;
  const int rc = run_device_loop(ctrl, view);
  ctrl.shutdown();
  return rc;
#else
  return run_sdl_loop(ctrl, view, cli.scale);
#endif
}
