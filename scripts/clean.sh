#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

echo "── 清理 build 目录 ──"
rm -rf "$BUILD_DIR"
echo "完成。"
