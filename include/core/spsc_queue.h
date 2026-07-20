#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>

namespace minitrader {

/// Hardware cache line size (typically 64 bytes on x86/ARM).
inline constexpr std::size_t kCacheLineSize = 64;

/// Lock-free Single-Producer Single-Consumer (SPSC) ring buffer.
///
/// Design choices:
/// - Power-of-2 capacity for branchless modulo (bitwise AND)
/// - Cache-line padding between head/tail to prevent false sharing
/// - memory_order_acquire/release for minimal fence overhead
/// - No dynamic allocation after construction (pre-allocated storage)
///
/// @tparam T      Element type (must be trivially copyable for optimal perf)
/// @tparam Cap    Capacity (must be power of 2)
template <typename T, std::size_t Cap>
    requires (Cap > 0 && (Cap & (Cap - 1)) == 0)  // power of 2
class SPSCQueue {
public:
    SPSCQueue() noexcept : head_(0), tail_(0) {}

    // Non-copyable, non-movable (pinned in memory for cache alignment)
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    /// Try to enqueue an element (producer side).
    /// @return true if successful, false if queue is full.
    template <typename... Args>
    [[nodiscard]] bool try_push(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args...>)
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next_tail = (tail + 1) & kMask;

        // If next_tail == head, the queue is full
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        // Construct element in-place
        new (&storage_[tail]) T(std::forward<Args>(args)...);

        // Publish: make the element visible to the consumer
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /// Try to dequeue an element (consumer side).
    /// @return The element if available, std::nullopt if queue is empty.
    [[nodiscard]] std::optional<T> try_pop() noexcept(
        std::is_nothrow_move_constructible_v<T>)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        // If head == tail, queue is empty
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        // Read element
        T* elem = reinterpret_cast<T*>(&storage_[head]);
        std::optional<T> result(std::move(*elem));
        elem->~T();

        // Advance head: signal to producer that slot is free
        head_.store((head + 1) & kMask, std::memory_order_release);
        return result;
    }

    /// Check if the queue is empty (approximate, for diagnostics only).
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    /// Approximate size (for diagnostics only, not thread-safe).
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const auto h = head_.load(std::memory_order_relaxed);
        const auto t = tail_.load(std::memory_order_relaxed);
        return (t - h) & kMask;
    }

    static constexpr std::size_t capacity() noexcept { return Cap - 1; }

private:
    static constexpr std::size_t kMask = Cap - 1;

    // Cache-line aligned and padded to prevent false sharing
    alignas(kCacheLineSize) std::atomic<std::size_t> head_;
    char pad1_[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    alignas(kCacheLineSize) std::atomic<std::size_t> tail_;
    char pad2_[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    // Storage: aligned array of raw bytes for placement new
    alignas(kCacheLineSize)
        std::aligned_storage_t<sizeof(T), alignof(T)> storage_[Cap];
};

}  // namespace minitrader
