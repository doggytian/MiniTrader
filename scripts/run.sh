#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

if [ ! -f "$BUILD_DIR/minitrader_demo" ]; then
    echo "未找到可执行文件，先执行编译..."
    "$PROJECT_ROOT/scripts/build.sh"
fi

echo "── 运行 MiniTrader Demo ──"
"$BUILD_DIR/minitrader_demo"
