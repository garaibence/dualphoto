#!/usr/bin/env bash
# Usage: scripts/build.sh [Release|Debug] [extra cmake args...]
set -euo pipefail
cd "$(dirname "$0")/.."
type="${1:-Release}"; shift || true
cmake -S . -B build -DCMAKE_BUILD_TYPE="$type" "$@"
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure || true
