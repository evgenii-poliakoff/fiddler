#!/usr/bin/env bash
# Run the full ctest suite. Used by humans, CI, and Claude/agents
# uniformly so we never bikeshed the invocation.
#
# Env overrides:
#   BUILD_DIR  default "build"
#
# Pass-through: any extra args are forwarded to ctest, e.g.
#   ./scripts/test.sh -R 'WaveformWidget' --output-on-failure

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "No build directory at '$BUILD_DIR' — run ./scripts/bootstrap.sh first." >&2
    exit 1
fi

ctest --test-dir "$BUILD_DIR" --output-on-failure "$@"
