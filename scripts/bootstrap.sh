#!/usr/bin/env bash
# Configure and build the project in ./build using Ninja. Used by
# humans, CI, and Claude/agents uniformly so the invocation is
# always the same.
#
# Env overrides:
#   BUILD_DIR   default "build"
#   BUILD_TYPE  default "Debug"
#
# Companion scripts (use these instead of typing raw commands):
#   ./scripts/install-deps.sh   — apt deps for Ubuntu 24.04
#   ./scripts/test.sh           — run the ctest suite
#   ./scripts/screenshots.sh    — regenerate docs/img/*.png

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j

echo
echo "Build complete."
echo "  Run the app:     ./$BUILD_DIR/src/fiddler"
echo "  Run tests:       ./scripts/test.sh"
echo "  Regenerate PNGs: ./scripts/screenshots.sh"
