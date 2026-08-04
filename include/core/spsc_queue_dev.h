#pragma once

#include <atomic>
#include <optional>

template <typename T, size_t Cap>
class MySPSCQueue {
public:
    static_assert(Cap > 0 && (Cap & (Cap - 1)) == 0, "Cap must be a power of 2");

    // 生产者
    bool try_push(T val) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) >= Cap) {
            return false; // 队列已满
        }
        buf_[t & (Cap - 1)] = std::move(val);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // 消费者
    std::optional<T> try_pop() {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (tail_.load(std::memory_order_acquire) == h) {
            return std::nullopt;  // 队列已空
        }
        T val = std::move(buf_[h & (Cap - 1)]);
        head_.store(h + 1, std::memory_order_release);
        return val;
    }

private:
    alignas(64) std::atomic<std::size_t> head_{0};
    char pad_1[64 - sizeof(std::atomic<std::size_t>)];
    alignas(64) std::atomic<std::size_t> tail_{0};
    char pad_2[64 - sizeof(std::atomic<std::size_t>)];
    T buf_[Cap];
};
