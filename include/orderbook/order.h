#pragma once

#include <cstdint>
#include <chrono>

namespace minitrader {

enum class Side : uint8_t {
    Buy = 0,
    Sell = 1,
};

enum class OrderType : uint8_t {
    Limit = 0,
    Market = 1,
    Cancel = 2,
};

/// Compact order representation (fits in a cache line).
/// All prices are in integer ticks to avoid floating-point.
struct Order {
    uint64_t order_id;      // Unique order identifier
    uint64_t instrument_id; // Instrument/symbol identifier
    int64_t  price;         // Price in ticks (0 for market orders)
    int32_t  quantity;      // Remaining quantity
    Side     side;
    OrderType type;
    uint16_t reserved{0};   // Padding for alignment
    uint64_t timestamp_ns;  // Nanosecond timestamp (clock_gettime)

    /// Get current timestamp in nanoseconds.
    static uint64_t now_ns() noexcept {
        auto tp = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                tp.time_since_epoch()).count());
    }
};

static_assert(sizeof(Order) <= 64, "Order must fit in a cache line");

/// Execution report sent back after matching.
struct ExecutionReport {
    uint64_t order_id;
    uint64_t matched_order_id;
    int64_t  price;
    int32_t  filled_quantity;
    Side     side;
    bool     is_maker;       // true if this order was resting in the book
    uint64_t timestamp_ns;
};

}  // namespace minitrader
