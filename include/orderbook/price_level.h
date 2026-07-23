#pragma once

#include <cstdint>
#include <list>
#include "orderbook/order.h"

namespace minitrader {

/// A single price level in the order book.
/// Orders at the same price are stored in FIFO (time priority).
///
/// Uses std::list instead of std::deque so that iterators remain stable
/// across insertions — required for the O(1) cancel_order implementation
/// in OrderBook (which caches iterators in an order-id map).
struct PriceLevel {
    int64_t price{0};
    int32_t total_quantity{0};
    std::list<Order> orders;  // FIFO queue of resting orders

    bool empty() const noexcept { return orders.empty(); }

    void add_order(Order order) {
        total_quantity += order.quantity;
        orders.push_back(std::move(order));
    }

    /// Remove the front order (after full fill).
    void remove_front() {
        if (!orders.empty()) {
            total_quantity -= orders.front().quantity;
            orders.pop_front();
        }
    }

    /// Reduce front order quantity (partial fill).
    void reduce_front(int32_t filled) {
        if (!orders.empty()) {
            orders.front().quantity -= filled;
            total_quantity -= filled;
        }
    }

    /// Erase an arbitrary order by iterator (O(1) for std::list).
    /// Caller must update total_quantity before calling.
    void erase(std::list<Order>::iterator it) {
        total_quantity -= it->quantity;
        orders.erase(it);
    }
};

}  // namespace minitrader
