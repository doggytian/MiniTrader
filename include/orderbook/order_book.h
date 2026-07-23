#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>
#include "orderbook/order.h"
#include "orderbook/price_level.h"

namespace minitrader {

/// Configuration for OrderBook.
struct OrderBookConfig {
    int64_t min_price{0};       // Minimum possible price (in ticks)
    int64_t max_price{10000};   // Maximum possible price (in ticks)
    int64_t tick_size{1};       // Price tick granularity
};

/// Flat-array indexed order book with O(1) price level access.
///
/// Design rationale:
/// - std::map (red-black tree): O(log N) but pointer chasing kills cache
/// - Flat array: O(1) access, perfect spatial locality for dense price ranges
/// - Trade-off: uses more memory for sparse books (acceptable for most instruments)
///
/// cancel_order is O(1) via an order-id → (side, price_index, list::iterator)
/// lookup table.  PriceLevel uses std::list so iterators survive insertions.
class OrderBook {
public:
    using FillCallback = std::function<void(const ExecutionReport&)>;

    explicit OrderBook(OrderBookConfig config);

    /// Add a new order to the book (may trigger matching).
    void add_order(Order order);

    /// Cancel an order by ID. O(1) via internal lookup table.
    /// @return true if found and cancelled, false if order not found.
    bool cancel_order(uint64_t order_id);

    /// Get the best bid price (highest buy), or -1 if no bids.
    [[nodiscard]] int64_t best_bid() const noexcept;

    /// Get the best ask price (lowest sell), or -1 if no asks.
    [[nodiscard]] int64_t best_ask() const noexcept;

    /// Get the spread in ticks (ask - bid), or -1 if one side is empty.
    [[nodiscard]] int64_t spread() const noexcept;

    /// Get total quantity at a given price level.
    [[nodiscard]] int32_t quantity_at(int64_t price, Side side) const noexcept;

    /// Register callback for execution reports (fills).
    void set_fill_callback(FillCallback cb) { fill_callback_ = std::move(cb); }

private:
    /// Location of a resting order — enough to find and erase it in O(1).
    struct OrderLocation {
        Side side;
        std::size_t price_idx;
        std::list<Order>::iterator it;
    };

    /// Convert price to array index.
    [[nodiscard]] std::size_t price_to_index(int64_t price) const noexcept {
        return static_cast<std::size_t>((price - config_.min_price) / config_.tick_size);
    }

    /// Try to match incoming order against resting orders.
    void match(Order& incoming);

    /// Emit fill report via callback.
    void emit_fill(const ExecutionReport& report);

    /// Scan for the new best bid after a level is exhausted.
    void update_best_bid_after_empty(std::size_t exhausted_idx) noexcept;

    /// Scan for the new best ask after a level is exhausted.
    void update_best_ask_after_empty(std::size_t exhausted_idx) noexcept;

    OrderBookConfig config_;
    std::vector<PriceLevel> bids_;   // Indexed by price
    std::vector<PriceLevel> asks_;   // Indexed by price

    int64_t best_bid_{-1};
    int64_t best_ask_{-1};

    /// O(1) cancel lookup: order_id → location in bids_/asks_.
    std::unordered_map<uint64_t, OrderLocation> order_map_;

    FillCallback fill_callback_;
};

}  // namespace minitrader
