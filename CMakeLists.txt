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

# Qt 6 — Widgets for the app, Test for QSignalSpy / QTest in the
# GUI test suite.
find_package(Qt6 REQUIRED COMPONENTS Widgets Test)
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

# Rubber Band Library — real-time time-stretching engine. Ships
# rubberband.pc on Ubuntu 24.04 (librubberband-dev), so pkg-config is
# the canonical integration point. See Rule 3 in docs/architecture.md.
pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
