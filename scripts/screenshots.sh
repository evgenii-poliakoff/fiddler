#!/usr/bin/env bash
# Regenerate the user manual's PNGs by running fiddler-screenshots
# against the scene manifest at tools/screenshots/scenes.yaml.
# Output lands in docs/img/. Builds the tool first if needed.
#
# Used by:
#   * humans, after a UI change that affects a documented control
#   * CI, to detect drift between repo PNGs and freshly generated ones
#   * Claude/agents, as the canonical regenerate command
#
# Env overrides:
#   BUILD_DIR  default "build"

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"

if [[ ! -d "$BUILD_DIR" ]]; then
    echo "No build directory at '$BUILD_DIR' — run ./scripts/bootstrap.sh first." >&2
    exit 1
fi

# Build only the screenshots tool target (cheap if up to date).
cmake --build "$BUILD_DIR" --target fiddler-screenshots

# Defaults (--output, --manifest) are baked at configure time and
# point at this repo's docs/img/ + tools/screenshots/scenes.yaml,
# so a bare invocation does the right thing.
"./$BUILD_DIR/tools/screenshots/fiddler-screenshots" "$@"
