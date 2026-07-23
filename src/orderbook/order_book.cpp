#include "orderbook/order_book.h"

namespace minitrader {

OrderBook::OrderBook(OrderBookConfig config)
    : config_(config)
{
    const std::size_t num_levels =
        static_cast<std::size_t>((config_.max_price - config_.min_price) / config_.tick_size) + 1;
    bids_.resize(num_levels);
    asks_.resize(num_levels);

    // 初始化每个价位的价格字段
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

    // 先尝试撮合
    match(order);

    // 有剩余量的限价单挂入订单簿
    if (order.quantity > 0 && order.type == OrderType::Limit) {
        const auto idx = price_to_index(order.price);
        if (order.side == Side::Buy) {
            auto& level = bids_[idx];
            level.add_order(order);  // 传值拷贝，order_id 仍可用
            // 登记位置，供 O(1) 撤单使用
            order_map_[order.order_id] = {Side::Buy, idx, std::prev(level.orders.end())};
            if (best_bid_ < 0 || order.price > best_bid_) {
                best_bid_ = order.price;
            }
        } else {
            auto& level = asks_[idx];
            level.add_order(order);
            order_map_[order.order_id] = {Side::Sell, idx, std::prev(level.orders.end())};
            if (best_ask_ < 0 || order.price < best_ask_) {
                best_ask_ = order.price;
            }
        }
    }
}

bool OrderBook::cancel_order(uint64_t order_id) {
    auto map_it = order_map_.find(order_id);
    if (map_it == order_map_.end()) {
        return false;  // 订单不存在（已成交或从未存在）
    }

    // 先值拷贝位置信息，再 erase map entry
    // （erase 会使 map_it 失效，不能再通过引用访问）
    const OrderLocation loc = map_it->second;
    order_map_.erase(map_it);

    auto& level = (loc.side == Side::Buy) ? bids_[loc.price_idx] : asks_[loc.price_idx];

    // 通过缓存迭代器 O(1) 删除 list 节点
    level.erase(loc.it);

    // 若撤单后该价位清空，更新最优价
    if (loc.side == Side::Buy) {
        if (level.empty() && level.price == best_bid_) {
            update_best_bid_after_empty(loc.price_idx);
        }
    } else {
        if (level.empty() && level.price == best_ask_) {
            update_best_ask_after_empty(loc.price_idx);
        }
    }

    return true;
}

int64_t OrderBook::best_bid() const noexcept { return best_bid_; }
int64_t OrderBook::best_ask() const noexcept { return best_ask_; }

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
        // 买单与卖单撮合（从最低卖价开始）
        while (incoming.quantity > 0 && best_ask_ >= 0 && incoming.price >= best_ask_) {
            auto idx = price_to_index(best_ask_);
            auto& level = asks_[idx];

            while (!level.empty() && incoming.quantity > 0) {
                auto& resting = level.orders.front();
                int32_t fill_qty = std::min(incoming.quantity, resting.quantity);

                // 向买卖双方各发一条成交回报
                emit_fill({incoming.order_id, resting.order_id, best_ask_,
                           fill_qty, Side::Buy, false, Order::now_ns()});
                emit_fill({resting.order_id, incoming.order_id, best_ask_,
                           fill_qty, Side::Sell, true, Order::now_ns()});

                incoming.quantity -= fill_qty;
                if (fill_qty >= resting.quantity) {
                    // 挂单完全成交：从 map 和价位队列中移除
                    order_map_.erase(resting.order_id);
                    level.remove_front();
                } else {
                    level.reduce_front(fill_qty);
                }
            }

            if (level.empty()) {
                update_best_ask_after_empty(idx);
            }
        }
    } else {
        // 卖单与买单撮合（从最高买价开始）
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
                    order_map_.erase(resting.order_id);
                    level.remove_front();
                } else {
                    level.reduce_front(fill_qty);
                }
            }

            if (level.empty()) {
                update_best_bid_after_empty(idx);
            }
        }
    }
}

void OrderBook::emit_fill(const ExecutionReport& report) {
    if (fill_callback_) {
        fill_callback_(report);
    }
}

void OrderBook::update_best_bid_after_empty(std::size_t exhausted_idx) noexcept {
    best_bid_ = -1;
    // 从清空价位向下扫，找到下一个非空买单价位
    for (int i = static_cast<int>(exhausted_idx) - 1; i >= 0; --i) {
        auto& b = bids_[static_cast<std::size_t>(i)];
        if (!b.empty()) {
            best_bid_ = b.price;
            break;
        }
    }
}

void OrderBook::update_best_ask_after_empty(std::size_t exhausted_idx) noexcept {
    best_ask_ = -1;
    // 从清空价位向上扫，找到下一个非空卖单价位
    for (auto i = exhausted_idx + 1; i < asks_.size(); ++i) {
        if (!asks_[i].empty()) {
            best_ask_ = asks_[i].price;
            break;
        }
    }
}

}  // namespace minitrader
