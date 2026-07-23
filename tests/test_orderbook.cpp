#include <gtest/gtest.h>
#include "orderbook/order_book.h"

using namespace minitrader;

class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        OrderBookConfig config{.min_price = 100, .max_price = 200, .tick_size = 1};
        book = std::make_unique<OrderBook>(config);
    }

    std::unique_ptr<OrderBook> book;
    uint64_t next_id{1};

    Order make_order(Side side, int64_t price, int32_t qty) {
        return Order{
            .order_id      = next_id++,
            .instrument_id = 1,
            .price         = price,
            .quantity      = qty,
            .side          = side,
            .type          = OrderType::Limit,
            .timestamp_ns  = Order::now_ns(),
        };
    }
};

// 空订单簿：最优价和价差均为 -1
TEST_F(OrderBookTest, EmptyBook) {
    EXPECT_EQ(book->best_bid(), -1);
    EXPECT_EQ(book->best_ask(), -1);
    EXPECT_EQ(book->spread(), -1);
}

// 挂买单后最优买价更新
TEST_F(OrderBookTest, AddBidUpdatesbestBid) {
    book->add_order(make_order(Side::Buy, 150, 10));
    EXPECT_EQ(book->best_bid(), 150);
    EXPECT_EQ(book->quantity_at(150, Side::Buy), 10);
}

// 挂卖单后最优卖价更新
TEST_F(OrderBookTest, AddAskUpdatesBestAsk) {
    book->add_order(make_order(Side::Sell, 160, 5));
    EXPECT_EQ(book->best_ask(), 160);
    EXPECT_EQ(book->quantity_at(160, Side::Sell), 5);
}

// 价差计算正确
TEST_F(OrderBookTest, SpreadCalculation) {
    book->add_order(make_order(Side::Buy, 150, 10));
    book->add_order(make_order(Side::Sell, 155, 10));
    EXPECT_EQ(book->spread(), 5);
}

// 完全成交：双方各收一条回报，卖方价位清空
TEST_F(OrderBookTest, MatchingFullFill) {
    int fill_count = 0;
    book->set_fill_callback([&](const ExecutionReport& report) {
        fill_count++;
        EXPECT_EQ(report.filled_quantity, 5);
        EXPECT_EQ(report.price, 150);
    });

    book->add_order(make_order(Side::Sell, 150, 5));  // 挂卖单 @ 150
    book->add_order(make_order(Side::Buy,  150, 5));  // 进场买单触发撮合

    EXPECT_EQ(fill_count, 2);          // 买卖双方各一条回报
    EXPECT_EQ(book->best_ask(), -1);   // 卖单已清空
}

// 部分成交：挂单剩余量正确
TEST_F(OrderBookTest, MatchingPartialFill) {
    book->add_order(make_order(Side::Sell, 150, 10));  // 挂卖单 10 手 @ 150
    book->add_order(make_order(Side::Buy,  150,  3));  // 只买 3 手

    EXPECT_EQ(book->quantity_at(150, Side::Sell), 7);  // 剩余 7 手
    EXPECT_EQ(book->best_ask(), 150);
}

// 撤单基础功能：撤后价位量为 0，最优价更新
TEST_F(OrderBookTest, CancelOrderBasic) {
    auto order = make_order(Side::Buy, 150, 10);
    uint64_t id = order.order_id;
    book->add_order(std::move(order));

    EXPECT_EQ(book->best_bid(), 150);
    EXPECT_TRUE(book->cancel_order(id));
    EXPECT_EQ(book->quantity_at(150, Side::Buy), 0);
    EXPECT_EQ(book->best_bid(), -1);   // 该价位已空
}

// 撤销最优价挂单后，最优价回退到次优价
TEST_F(OrderBookTest, CancelOrderUpdatesBestPrice) {
    auto o1 = make_order(Side::Buy, 155, 10);  // 较优买价
    auto o2 = make_order(Side::Buy, 150, 10);  // 次优买价
    uint64_t id1 = o1.order_id;
    book->add_order(std::move(o1));
    book->add_order(std::move(o2));

    EXPECT_EQ(book->best_bid(), 155);
    EXPECT_TRUE(book->cancel_order(id1));
    EXPECT_EQ(book->best_bid(), 150);  // 回退到次优价
}

// 撤不存在的订单返回 false
TEST_F(OrderBookTest, CancelOrderNotFound) {
    EXPECT_FALSE(book->cancel_order(9999));
}

// 已成交订单无法再撤（从簿中消失）
TEST_F(OrderBookTest, CancelAlreadyFilledOrder) {
    auto ask = make_order(Side::Sell, 150, 5);
    auto bid = make_order(Side::Buy,  150, 5);
    uint64_t ask_id = ask.order_id;
    book->add_order(std::move(ask));
    book->add_order(std::move(bid));  // 完全成交，ask 从簿中消失

    EXPECT_FALSE(book->cancel_order(ask_id));
}

// 价格-时间优先：同价位先挂先成交
TEST_F(OrderBookTest, PriceTimePriority) {
    book->add_order(make_order(Side::Sell, 150, 5));   // 订单 A（id=1）
    book->add_order(make_order(Side::Sell, 150, 5));   // 订单 B（id=2）

    uint64_t first_matched = 0;
    book->set_fill_callback([&](const ExecutionReport& report) {
        if (report.is_maker && first_matched == 0) {
            first_matched = report.order_id;
        }
    });

    book->add_order(make_order(Side::Buy, 150, 3));    // 应先撮合订单 A
    EXPECT_EQ(first_matched, 1);  // 订单 A（id=1）先成交
}
