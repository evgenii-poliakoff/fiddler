#!/usr/bin/env bash
# Install all build dependencies on Ubuntu 24.04.
#
# Note on `apt-get update`: we deliberately don't fail the script if it
# returns non-zero. On developer machines, `update` often complains
# about unrelated third-party PPAs (broken, retired, behind a VPN, etc.)
# that have nothing to do with this project. Our packages all live in
# the official Ubuntu repos; if any of those genuinely fail to refresh,
# the subsequent `apt-get install` will fail loudly with a clear error.

set -euo pipefail

sudo apt-get update || echo "Warning: apt-get update reported errors (likely unrelated third-party repos); continuing."

sudo apt-get install -y \
    build-essential cmake ninja-build pkg-config git \
    qt6-base-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev \
    portaudio19-dev \
    librubberband-dev \
    libspdlog-dev \
    catch2

echo "Dependencies installed."
