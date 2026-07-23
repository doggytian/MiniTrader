#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

if [ ! -f "$BUILD_DIR/engine_bench" ]; then
    echo "Benchmarks not built, building first..."
    "$PROJECT_ROOT/scripts/build.sh"
fi

echo "── SPSC Queue Benchmark ──"
"$BUILD_DIR/spsc_bench"
echo ""
echo "── OrderBook Benchmark ──"
"$BUILD_DIR/orderbook_bench"
echo ""
echo "── Engine Latency Benchmark (with P99 histogram) ──"
"$BUILD_DIR/engine_bench"
