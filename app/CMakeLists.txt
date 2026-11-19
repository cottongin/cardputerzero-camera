cmake_minimum_required(VERSION 3.16)
project(czcamera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CZ_BUILD_APP "Build czcamera application binary" OFF)
option(CZ_TARGET_DEVICE "Build for CardputerZero Linux fbdev/evdev target" OFF)
option(CZ_BUILD_TESTS "Build czcamera unit tests" ON)
option(CZ_ENABLE_LVGL "Enable optional LVGL UI hook if available" OFF)

find_package(JPEG QUIET)
if(NOT JPEG_FOUND)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBJPEG IMPORTED_TARGET libjpeg)
  endif()
  if(TARGET PkgConfig::LIBJPEG)
    add_library(JPEG::JPEG ALIAS PkgConfig::LIBJPEG)
  else()
    find_package(JPEG REQUIRED)
  endif()
endif()
find_package(Freetype QUIET)
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

if(Freetype_FOUND)
  target_compile_definitions(czcamera_core PUBLIC CZ_HAS_FREETYPE=1)
  target_link_libraries(czcamera_core PUBLIC Freetype::Freetype)
endif()

if(CZ_ENABLE_LVGL AND lvgl_FOUND)
  target_compile_definitions(czcamera_core PUBLIC CP0_HAS_LVGL=1)
  target_link_libraries(czcamera_core PUBLIC lvgl::lvgl)
endif()

if(CZ_BUILD_APP)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    message(FATAL_ERROR "CZ_BUILD_APP=ON requires src/main.cpp")
  endif()
  add_executable(czcamera src/main.cpp)
  target_compile_options(czcamera PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(czcamera PRIVATE czcamera_core JPEG::JPEG)
  if(CZ_TARGET_DEVICE)
    target_compile_definitions(czcamera PRIVATE CZ_TARGET_DEVICE=1)
  else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(czcamera PRIVATE SDL2::SDL2)
  endif()
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
endif()
