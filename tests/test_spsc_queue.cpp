#include <gtest/gtest.h>
#include <thread>
#include "core/spsc_queue.h"

using namespace minitrader;

// 基础入队出队测试
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

// 队列满时 try_push 返回 false
TEST(SPSCQueueTest, FullQueue) {
    SPSCQueue<int, 4> q;  // 实际容量 = 3（保留一个槽位）

    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.try_push(4));  // 队列已满
}

// 空队列 try_pop 返回 nullopt
TEST(SPSCQueueTest, EmptyPop) {
    SPSCQueue<int, 8> q;
    auto val = q.try_pop();
    EXPECT_FALSE(val.has_value());
}

// 先进先出顺序验证
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

// 真并发：100 万次生产消费，验证无数据丢失和顺序正确
TEST(SPSCQueueTest, ConcurrentProducerConsumer) {
    constexpr int N = 1'000'000;
    SPSCQueue<int, 1024> q;

    std::thread producer([&]() {
        for (int i = 0; i < N; ++i) {
            while (!q.try_push(i)) {
                // 自旋等待空槽
            }
        }
    });

    std::thread consumer([&]() {
        for (int i = 0; i < N; ++i) {
            std::optional<int> val;
            while (!(val = q.try_pop())) {
                // 自旋等待数据
            }
            EXPECT_EQ(*val, i);
        }
    });

    producer.join();
    consumer.join();
    EXPECT_TRUE(q.empty());
}
