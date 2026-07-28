#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
DEMO_BIN="$BUILD_DIR/minitrader_demo"

# 默认回放文件（可传参覆盖）
CSV_PATH="${1:-$PROJECT_ROOT/data/sample_ticks.csv}"

if [ ! -f "$DEMO_BIN" ]; then
    echo "未找到可执行文件，先执行编译..."
    "$PROJECT_ROOT/scripts/build.sh"
fi

if [ ! -f "$CSV_PATH" ]; then
    echo "错误：回放文件不存在：$CSV_PATH"
    exit 1
fi

echo "── 运行 MiniTrader 行情回放（真实 tick 间隔驱动）──"
echo "文件：$CSV_PATH"
echo ""
"$DEMO_BIN" --recv "$CSV_PATH"
