cmake_minimum_required(VERSION 3.22)
project(fiddler
    VERSION 0.0.1
    DESCRIPTION "Transcription aid for self-taught fiddle players"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clangd / IDE tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings — turn them on early; cheap to keep clean from day zero.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# Qt 6 — Widgets is enough for step 1.
find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# FFmpeg + PortAudio via pkg-config (no upstream CMake config).
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG  REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)

# spdlog: Ubuntu's libspdlog-dev ships only a pkg-config file, no
# spdlogConfig.cmake. Pull it in via pkg-config — same pattern as
# FFmpeg and PortAudio above. The IMPORTED_TARGET creates
# PkgConfig::SPDLOG with the correct includes and link flags.
pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog)

# Rubber Band is wired in step 2; uncomment then.
# find_package(RubberBand REQUIRED)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
