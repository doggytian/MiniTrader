# MiniTrader

[![CI](https://github.com/tianxingyu/MiniTrader/actions/workflows/ci.yml/badge.svg)](https://github.com/tianxingyu/MiniTrader/actions/workflows/ci.yml)

A minimal, high-performance trading system built from scratch in modern C++20.  
Designed to demonstrate low-latency system design principles used in quantitative trading.

## Highlights

- **Lock-free SPSC queue** with `memory_order_acquire/release` — zero contention between market data and strategy threads
- **Cache-friendly OrderBook** using flat price-indexed array — no pointer chasing, no red-black tree
- **Event-driven architecture** — market data → strategy → risk check → order execution in a single deterministic path
- **Measured performance** — full-path P99 latency benchmarked with `perf` and flame graphs

## Architecture

```
┌─────────────┐     SPSC Queue     ┌─────────────┐     Risk Gate     ┌─────────────┐
│  Network    │ ──────────────────► │  Strategy   │ ────────────────► │   Order     │
│  (epoll)    │   lock-free, <50ns  │  Engine     │   inline check    │  Gateway    │
└─────────────┘                     └─────────────┘                   └─────────────┘
       ▲                                                                     │
       │                         ┌─────────────┐                             │
       └──── Market Data ◄────── │  Exchange   │ ◄───── Order Ack ───────────┘
                                 └─────────────┘
```

## Modules

| Module | Description | Key Technique |
|--------|-------------|---------------|
| `core/spsc_queue.h` | Lock-free single-producer single-consumer ring buffer | `std::atomic`, cache-line padding, `memory_order_acquire/release` |
| `orderbook/` | Price-time priority order book with flat array storage | Cache-friendly design, O(1) best bid/ask |
| `strategy/` | Strategy base class with backtest/live mode switch | Template Method pattern, zero-copy event passing |
| `network/` | epoll-based market data receiver | Edge-triggered, non-blocking IO, CPU affinity |
| `risk/` | Inline risk gate (cancel rate, self-trade, position limit) | Zero-overhead abstraction, compile-time policy |

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=20
cmake --build build -j$(nproc)
```

## Benchmark

```bash
./build/bench/spsc_bench        # SPSC queue throughput & latency
./build/bench/orderbook_bench   # OrderBook add/cancel/match
```

<!-- TODO: Add flame graph screenshot here -->

## Performance (measured on Apple M1 Pro, Release build)

| Metric | Value | Notes |
|--------|-------|-------|
| SPSC enqueue-dequeue round-trip | **2.1 ns** | Single-threaded, int64_t payload |
| SPSC round-trip (32-byte struct) | **2.5 ns** | |
| SPSC throughput (burst 8) | **434 M ops/s** | |
| OrderBook add\_order (single level) | **43 ns** | |
| OrderBook cancel\_order (O(1)) | **24 ns** | `unordered_map` lookup + `list::erase` |
| OrderBook match (depth ≥ 10) | **~450 ns** | 1 incoming vs N resting |
| **Full path: tick → strategy (no order)** | **65 ns** | SPSC dequeue + on_tick return |
| **Full path: tick → 2× order submit** | **735 ns** | + 2× risk check + 2× book insert |

### Latency Percentiles (100K tick steady-state)

| P50 | P90 | P99 | P99.9 | Max |
|-----|-----|-----|-------|-----|
| 83 ns | 84 ns | 125 ns | 167 ns | ~14 µs |

> Numbers above are single-threaded micro-benchmarks. Real-world latency depends on network stack and system load.

## Project Structure

```
MiniTrader/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── core/
│   │   └── spsc_queue.h        # Lock-free SPSC ring buffer
│   ├── orderbook/
│   │   ├── order.h             # Order types and definitions
│   │   ├── price_level.h       # Price level (bid/ask bucket)
│   │   └── order_book.h        # Order book with matching engine
│   ├── strategy/
│   │   └── strategy_base.h     # Strategy interface
│   ├── network/
│   │   └── market_receiver.h   # epoll-based market data receiver
│   └── risk/
│       └── risk_gate.h         # Inline risk checks
├── src/
│   ├── core/
│   ├── orderbook/
│   ├── strategy/
│   ├── network/
│   └── risk/
├── tests/                      # Unit tests (Google Test)
├── bench/                      # Micro-benchmarks (Google Benchmark)
└── docs/                       # Design notes, flame graphs
```

## Design Decisions

### Why flat array instead of `std::map` for OrderBook?

`std::map` (red-black tree) has O(log N) access but terrible cache behavior — each node is a heap allocation with pointer indirection. For an order book where price ticks are bounded and dense, a flat array indexed by `(price - min_price) / tick_size` gives O(1) access with perfect spatial locality.

### Why SPSC queue instead of `std::mutex`?

In a trading system, the market data thread and strategy thread form a natural producer-consumer pair. A lock-free SPSC queue eliminates contention entirely — the producer and consumer never touch the same cache line (with proper padding). This gives deterministic latency without any OS scheduling dependency.

### Why epoll edge-triggered?

Level-triggered `epoll` re-notifies on every `epoll_wait` if data remains in the buffer. Edge-triggered notifies only on state change, reducing syscall overhead. Combined with non-blocking reads until `EAGAIN`, this minimizes kernel transitions in the hot path.

## License

MIT
