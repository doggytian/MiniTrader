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
            .order_id = next_id++,
            .instrument_id = 1,
            .price = price,
            .quantity = qty,
            .side = side,
            .type = OrderType::Limit,
            .timestamp_ns = Order::now_ns(),
        };
    }
};

TEST_F(OrderBookTest, EmptyBook) {
    EXPECT_EQ(book->best_bid(), -1);
    EXPECT_EQ(book->best_ask(), -1);
    EXPECT_EQ(book->spread(), -1);
}

TEST_F(OrderBookTest, AddBidUpdatesbestBid) {
    book->add_order(make_order(Side::Buy, 150, 10));
    EXPECT_EQ(book->best_bid(), 150);
    EXPECT_EQ(book->quantity_at(150, Side::Buy), 10);
}

TEST_F(OrderBookTest, AddAskUpdatesBestAsk) {
    book->add_order(make_order(Side::Sell, 160, 5));
    EXPECT_EQ(book->best_ask(), 160);
    EXPECT_EQ(book->quantity_at(160, Side::Sell), 5);
}

TEST_F(OrderBookTest, SpreadCalculation) {
    book->add_order(make_order(Side::Buy, 150, 10));
    book->add_order(make_order(Side::Sell, 155, 10));
    EXPECT_EQ(book->spread(), 5);
}

TEST_F(OrderBookTest, MatchingFullFill) {
    int fill_count = 0;
    book->set_fill_callback([&](const ExecutionReport& report) {
        fill_count++;
        EXPECT_EQ(report.filled_quantity, 5);
        EXPECT_EQ(report.price, 150);
    });

    book->add_order(make_order(Side::Sell, 150, 5));  // Resting ask at 150
    book->add_order(make_order(Side::Buy, 150, 5));   // Incoming buy crosses

    EXPECT_EQ(fill_count, 2);  // Both sides get a fill report
    EXPECT_EQ(book->best_ask(), -1);  // Ask exhausted
}

TEST_F(OrderBookTest, MatchingPartialFill) {
    book->add_order(make_order(Side::Sell, 150, 10));  // Resting ask: 10 @ 150
    book->add_order(make_order(Side::Buy, 150, 3));    // Buy only 3

    EXPECT_EQ(book->quantity_at(150, Side::Sell), 7);  // 7 remaining
    EXPECT_EQ(book->best_ask(), 150);
}

TEST_F(OrderBookTest, CancelOrderBasic) {
    auto order = make_order(Side::Buy, 150, 10);
    uint64_t id = order.order_id;
    book->add_order(std::move(order));

    EXPECT_EQ(book->best_bid(), 150);
    EXPECT_TRUE(book->cancel_order(id));
    EXPECT_EQ(book->quantity_at(150, Side::Buy), 0);
    EXPECT_EQ(book->best_bid(), -1);  // Level now empty
}

TEST_F(OrderBookTest, CancelOrderUpdatesBestPrice) {
    // Two bids at different prices — cancel the better one
    auto o1 = make_order(Side::Buy, 155, 10);
    auto o2 = make_order(Side::Buy, 150, 10);
    uint64_t id1 = o1.order_id;
    book->add_order(std::move(o1));
    book->add_order(std::move(o2));

    EXPECT_EQ(book->best_bid(), 155);
    EXPECT_TRUE(book->cancel_order(id1));
    EXPECT_EQ(book->best_bid(), 150);  // Falls back to next level
}

TEST_F(OrderBookTest, CancelOrderNotFound) {
    EXPECT_FALSE(book->cancel_order(9999));  // Non-existent id
}

TEST_F(OrderBookTest, CancelAlreadyFilledOrder) {
    auto ask = make_order(Side::Sell, 150, 5);
    auto bid = make_order(Side::Buy,  150, 5);
    uint64_t ask_id = ask.order_id;
    book->add_order(std::move(ask));
    book->add_order(std::move(bid));  // Fully fills the ask

    // ask is gone from the book — cancel should return false
    EXPECT_FALSE(book->cancel_order(ask_id));
}

TEST_F(OrderBookTest, PriceTimePriority) {
    // Two orders at same price — first in should be filled first
    book->add_order(make_order(Side::Sell, 150, 5));   // Order A
    book->add_order(make_order(Side::Sell, 150, 5));   // Order B

    uint64_t first_matched = 0;
    book->set_fill_callback([&](const ExecutionReport& report) {
        if (report.is_maker && first_matched == 0) {
            first_matched = report.order_id;
        }
    });

    book->add_order(make_order(Side::Buy, 150, 3));    // Should match Order A
    EXPECT_EQ(first_matched, 1);  // Order A (id=1) matched first
}
