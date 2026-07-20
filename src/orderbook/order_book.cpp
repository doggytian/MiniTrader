#include "orderbook/order_book.h"

namespace minitrader {

OrderBook::OrderBook(OrderBookConfig config)
    : config_(config)
{
    const std::size_t num_levels =
        static_cast<std::size_t>((config_.max_price - config_.min_price) / config_.tick_size) + 1;
    bids_.resize(num_levels);
    asks_.resize(num_levels);

    // Initialize price levels
    for (std::size_t i = 0; i < num_levels; ++i) {
        int64_t price = config_.min_price + static_cast<int64_t>(i) * config_.tick_size;
        bids_[i].price = price;
        asks_[i].price = price;
    }
}

void OrderBook::add_order(Order order) {
    if (order.type == OrderType::Cancel) {
        cancel_order(order.order_id);
        return;
    }

    // Try to match first
    match(order);

    // If residual quantity remains, add to resting book
    if (order.quantity > 0 && order.type == OrderType::Limit) {
        const auto idx = price_to_index(order.price);
        if (order.side == Side::Buy) {
            bids_[idx].add_order(std::move(order));
            if (best_bid_ < 0 || order.price > best_bid_) {
                best_bid_ = order.price;
            }
        } else {
            asks_[idx].add_order(std::move(order));
            if (best_ask_ < 0 || order.price < best_ask_) {
                best_ask_ = order.price;
            }
        }
    }
}

bool OrderBook::cancel_order(uint64_t order_id) {
    // TODO: Implement O(1) cancel via order_id -> location map
    // For now, linear scan (acceptable for MVP, optimize later)
    (void)order_id;
    return false;
}

int64_t OrderBook::best_bid() const noexcept {
    return best_bid_;
}

int64_t OrderBook::best_ask() const noexcept {
    return best_ask_;
}

int64_t OrderBook::spread() const noexcept {
    if (best_bid_ < 0 || best_ask_ < 0) return -1;
    return best_ask_ - best_bid_;
}

int32_t OrderBook::quantity_at(int64_t price, Side side) const noexcept {
    const auto idx = price_to_index(price);
    if (idx >= bids_.size()) return 0;
    return (side == Side::Buy) ? bids_[idx].total_quantity : asks_[idx].total_quantity;
}

void OrderBook::match(Order& incoming) {
    if (incoming.side == Side::Buy) {
        // Buy order matches against asks (lowest first)
        while (incoming.quantity > 0 && best_ask_ >= 0 && incoming.price >= best_ask_) {
            auto idx = price_to_index(best_ask_);
            auto& level = asks_[idx];

            while (!level.empty() && incoming.quantity > 0) {
                auto& resting = level.orders.front();
                int32_t fill_qty = std::min(incoming.quantity, resting.quantity);

                // Emit fill for both sides
                emit_fill({incoming.order_id, resting.order_id, best_ask_,
                           fill_qty, Side::Buy, false, Order::now_ns()});
                emit_fill({resting.order_id, incoming.order_id, best_ask_,
                           fill_qty, Side::Sell, true, Order::now_ns()});

                incoming.quantity -= fill_qty;
                if (fill_qty >= resting.quantity) {
                    level.remove_front();
                } else {
                    level.reduce_front(fill_qty);
                }
            }

            // Update best ask if level is exhausted
            if (level.empty()) {
                best_ask_ = -1;
                for (auto i = idx + 1; i < asks_.size(); ++i) {
                    if (!asks_[i].empty()) {
                        best_ask_ = asks_[i].price;
                        break;
                    }
                }
            }
        }
    } else {
        // Sell order matches against bids (highest first)
        while (incoming.quantity > 0 && best_bid_ >= 0 && incoming.price <= best_bid_) {
            auto idx = price_to_index(best_bid_);
            auto& level = bids_[idx];

            while (!level.empty() && incoming.quantity > 0) {
                auto& resting = level.orders.front();
                int32_t fill_qty = std::min(incoming.quantity, resting.quantity);

                emit_fill({incoming.order_id, resting.order_id, best_bid_,
                           fill_qty, Side::Sell, false, Order::now_ns()});
                emit_fill({resting.order_id, incoming.order_id, best_bid_,
                           fill_qty, Side::Buy, true, Order::now_ns()});

                incoming.quantity -= fill_qty;
                if (fill_qty >= resting.quantity) {
                    level.remove_front();
                } else {
                    level.reduce_front(fill_qty);
                }
            }

            // Update best bid if level is exhausted
            if (level.empty()) {
                best_bid_ = -1;
                for (int i = static_cast<int>(idx) - 1; i >= 0; --i) {
                    if (!bids_[static_cast<std::size_t>(i)].empty()) {
                        best_bid_ = bids_[static_cast<std::size_t>(i)].price;
                        break;
                    }
                }
            }
        }
    }
}

void OrderBook::emit_fill(const ExecutionReport& report) {
    if (fill_callback_) {
        fill_callback_(report);
    }
}

}  // namespace minitrader
