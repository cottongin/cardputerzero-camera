cmake_minimum_required(VERSION 3.16)
project(czcamera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CZ_BUILD_APP "Build czcamera application binary" OFF)
option(CZ_TARGET_DEVICE "Build for CardputerZero Linux fbdev/evdev target" OFF)
option(CZ_BUILD_TESTS "Build czcamera unit tests" ON)
option(CZ_ENABLE_LVGL "Enable optional LVGL UI hook if available" OFF)

find_package(PkgConfig QUIET)
find_package(JPEG QUIET)
if(NOT JPEG_FOUND)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBJPEG IMPORTED_TARGET libjpeg)
  endif()
  if(TARGET PkgConfig::LIBJPEG)
    add_library(JPEG::JPEG ALIAS PkgConfig::LIBJPEG)
  else()
    find_package(JPEG REQUIRED)
  endif()
endif()

set(CZ_FREETYPE_TARGET "")
find_package(Freetype QUIET)
if(TARGET Freetype::Freetype)
  set(CZ_FREETYPE_TARGET Freetype::Freetype)
elseif(PkgConfig_FOUND)
  pkg_check_modules(FREETYPE QUIET IMPORTED_TARGET freetype2)
  if(TARGET PkgConfig::FREETYPE)
    set(CZ_FREETYPE_TARGET PkgConfig::FREETYPE)
  endif()
endif()
find_package(Threads REQUIRED)

if(CZ_ENABLE_LVGL)
  find_package(lvgl QUIET)
endif()

add_library(czcamera_core STATIC
  src/theme.cpp
  src/config.cpp
  src/storage.cpp
  src/filters.cpp
  src/jpeg_pipeline.cpp
  src/subprocess_backend.cpp
  src/sim_backend.cpp
  src/backlight.cpp
  src/timelapse_engine.cpp
  src/gallery_model.cpp
  src/video_player.cpp
  src/ui_controller.cpp
  src/app_ui.cpp
)

target_include_directories(czcamera_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(czcamera_core
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(czcamera_core
  PUBLIC
    Threads::Threads
    JPEG::JPEG
)

if(CZ_FREETYPE_TARGET)
  target_compile_definitions(czcamera_core PUBLIC CZ_HAS_FREETYPE=1)
  target_link_libraries(czcamera_core PUBLIC ${CZ_FREETYPE_TARGET})
  message(STATUS "FreeType2 enabled via ${CZ_FREETYPE_TARGET}")
else()
  message(STATUS "FreeType2 not found: using built-in 5x7 bitmap fallback")
endif()

if(CZ_ENABLE_LVGL AND lvgl_FOUND)
  target_compile_definitions(czcamera_core PUBLIC CP0_HAS_LVGL=1)
  target_link_libraries(czcamera_core PUBLIC lvgl::lvgl)
endif()

if(CZ_TARGET_DEVICE)
  target_compile_definitions(czcamera_core PUBLIC CZ_TARGET_DEVICE=1)
endif()

if(CZ_BUILD_APP)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    message(FATAL_ERROR "CZ_BUILD_APP=ON requires src/main.cpp")
  endif()
  add_executable(czcamera src/main.cpp)
  target_compile_options(czcamera PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(czcamera PRIVATE czcamera_core JPEG::JPEG)
  if(CZ_TARGET_DEVICE)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
      message(FATAL_ERROR "CZ_TARGET_DEVICE=ON requires Linux (fbdev/evdev); got CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
    endif()
    message(STATUS "Target backend: Linux fbdev + evdev")
  else()
    set(CZ_SDL2_TARGET "")
    if(PkgConfig_FOUND)
      pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
      if(TARGET PkgConfig::SDL2)
        set(CZ_SDL2_TARGET PkgConfig::SDL2)
      endif()
    endif()
    if(NOT CZ_SDL2_TARGET)
      find_package(SDL2 REQUIRED)
      if(TARGET SDL2::SDL2)
        set(CZ_SDL2_TARGET SDL2::SDL2)
      endif()
    endif()
    if(CZ_SDL2_TARGET)
      target_link_libraries(czcamera PRIVATE ${CZ_SDL2_TARGET})
    else()
      target_include_directories(czcamera PRIVATE ${SDL2_INCLUDE_DIRS})
      target_link_libraries(czcamera PRIVATE ${SDL2_LIBRARIES})
    endif()
    message(STATUS "Target backend: SDL2 320x170 simulator")
  endif()
  install(TARGETS czcamera RUNTIME DESTINATION bin)
endif()

if(CZ_BUILD_TESTS)
  enable_testing()
  add_executable(test_config_and_storage tests/test_config_and_storage.cpp)
  target_compile_options(test_config_and_storage PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(test_config_and_storage PRIVATE czcamera_core)
  add_test(NAME test_config_and_storage COMMAND test_config_and_storage)

  add_executable(test_filters_and_jpeg tests/test_filters_and_jpeg.cpp)
  target_compile_options(test_filters_and_jpeg PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(test_filters_and_jpeg PRIVATE czcamera_core)
  add_test(NAME test_filters_and_jpeg COMMAND test_filters_and_jpeg)

  add_executable(test_backend_protocol tests/test_backend_protocol.cpp)
  target_compile_options(test_backend_protocol PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_definitions(test_backend_protocol PRIVATE
    CZ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
  )
  target_link_libraries(test_backend_protocol PRIVATE czcamera_core)
  add_test(NAME test_backend_protocol COMMAND test_backend_protocol)

  add_executable(test_timelapse_and_gallery tests/test_timelapse_and_gallery.cpp)
  target_compile_options(test_timelapse_and_gallery PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(test_timelapse_and_gallery PRIVATE czcamera_core)
  add_test(NAME test_timelapse_and_gallery COMMAND test_timelapse_and_gallery)

  add_executable(test_ui_controller tests/test_ui_controller.cpp)
  target_compile_options(test_ui_controller PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(test_ui_controller PRIVATE czcamera_core)
  add_test(NAME test_ui_controller COMMAND test_ui_controller)
endif()
