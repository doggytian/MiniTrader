#include <gtest/gtest.h>
#include <thread>
#include "core/spsc_queue.h"

using namespace minitrader;

TEST(SPSCQueueTest, BasicPushPop) {
    SPSCQueue<int, 16> q;

    EXPECT_TRUE(q.empty());
    EXPECT_TRUE(q.try_push(42));
    EXPECT_FALSE(q.empty());

    auto val = q.try_pop();
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, 42);
    EXPECT_TRUE(q.empty());
}

TEST(SPSCQueueTest, FullQueue) {
    SPSCQueue<int, 4> q;  // Capacity = 3 (one slot reserved)

    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.try_push(4));  // Full!
}

TEST(SPSCQueueTest, EmptyPop) {
    SPSCQueue<int, 8> q;
    auto val = q.try_pop();
    EXPECT_FALSE(val.has_value());
}

TEST(SPSCQueueTest, FIFO_Order) {
    SPSCQueue<int, 64> q;

    for (int i = 0; i < 50; ++i) {
        EXPECT_TRUE(q.try_push(i));
    }
    for (int i = 0; i < 50; ++i) {
        auto val = q.try_pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, i);
    }
}

TEST(SPSCQueueTest, ConcurrentProducerConsumer) {
    constexpr int N = 1'000'000;
    SPSCQueue<int, 1024> q;

    std::thread producer([&]() {
        for (int i = 0; i < N; ++i) {
            while (!q.try_push(i)) {
                // Spin until space available
            }
        }
    });

    std::thread consumer([&]() {
        for (int i = 0; i < N; ++i) {
            std::optional<int> val;
            while (!(val = q.try_pop())) {
                // Spin until data available
            }
            EXPECT_EQ(*val, i);
        }
    });

    producer.join();
    consumer.join();
    EXPECT_TRUE(q.empty());
}
