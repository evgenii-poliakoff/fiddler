#!/usr/bin/env bash
# Configure and build in ./build using Ninja.
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

cmake -S . -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j

echo
echo "Build complete. Run with:"
echo "  ./$BUILD_DIR/src/fiddler"
echo
echo "Run tests with:"
echo "  ctest --test-dir $BUILD_DIR --output-on-failure"
