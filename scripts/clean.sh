#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

echo "── Cleaning build directory ──"
rm -rf "$BUILD_DIR"
echo "Done."
