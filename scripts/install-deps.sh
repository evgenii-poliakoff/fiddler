#!/usr/bin/env bash
# Install all build dependencies on Ubuntu 24.04.
set -euo pipefail

sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build pkg-config git \
    qt6-base-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev \
    portaudio19-dev \
    librubberband-dev \
    catch2

echo "Dependencies installed."
