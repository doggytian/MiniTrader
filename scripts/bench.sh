#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

if [ ! -f "$BUILD_DIR/engine_bench" ]; then
    echo "未找到 benchmark 文件，先执行编译..."
    "$PROJECT_ROOT/scripts/build.sh"
fi

echo "── SPSC 队列 Benchmark ──"
"$BUILD_DIR/spsc_bench"
echo ""
echo "── OrderBook Benchmark ──"
"$BUILD_DIR/orderbook_bench"
echo ""
echo "── 引擎全链路延迟 Benchmark（含 P99 histogram）──"
"$BUILD_DIR/engine_bench"
