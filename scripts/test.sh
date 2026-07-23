#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

if [ ! -f "$BUILD_DIR/test_orderbook" ]; then
    echo "Tests not built, building first..."
    "$PROJECT_ROOT/scripts/build.sh"
fi

echo "── Running Tests ──"
"$BUILD_DIR/test_spsc_queue"
"$BUILD_DIR/test_orderbook"
echo "── All tests passed ──"
