#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
BUILD_TYPE="${1:-Release}"

echo "── 编译 MiniTrader（$BUILD_TYPE）──"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$PROJECT_ROOT"
cmake --build "$BUILD_DIR" --parallel
echo "── 编译完成 ──"
