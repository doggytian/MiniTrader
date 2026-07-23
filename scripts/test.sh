#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

if [ ! -f "$BUILD_DIR/test_orderbook" ]; then
    echo "未找到测试文件，先执行编译..."
    "$PROJECT_ROOT/scripts/build.sh"
fi

echo "── 运行单元测试 ──"
"$BUILD_DIR/test_spsc_queue"
"$BUILD_DIR/test_orderbook"
echo "── 全部测试通过 ──"
