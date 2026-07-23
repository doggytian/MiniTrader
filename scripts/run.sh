#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

if [ ! -f "$BUILD_DIR/minitrader_demo" ]; then
    echo "Binary not found, building first..."
    "$PROJECT_ROOT/scripts/build.sh"
fi

echo "── Running MiniTrader Demo ──"
"$BUILD_DIR/minitrader_demo"
