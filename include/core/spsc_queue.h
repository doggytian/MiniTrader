#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>

namespace minitrader {

/// 硬件 cache line 大小（x86/ARM 通常为 64 字节）。
inline constexpr std::size_t kCacheLineSize = 64;

/// 无锁单生产者单消费者（SPSC）环形队列。
///
/// 设计决策：
/// - 容量为 2 的幂，用位与代替取模（无分支）
/// - head/tail 之间插入 cache-line 填充，防止伪共享
/// - memory_order_acquire/release 实现最小化内存屏障开销
/// - 构造后不再动态分配内存（预分配存储）
///
/// 存储层使用 `alignas(T) std::byte[]` 而非已废弃的
/// `std::aligned_storage_t<>`（C++23 P1413R3 移除），
/// std::byte 无别名限制，且 sizeof(storage_) 精确等于 sizeof(T)*Cap，
/// 无对齐舍入带来的意外行为。
///
/// @tparam T    元素类型（trivially copyable 可获得最优性能）
/// @tparam Cap  队列容量（必须为 2 的幂）
template <typename T, std::size_t Cap>
    requires (Cap > 0 && (Cap & (Cap - 1)) == 0)  // 必须为 2 的幂
class SPSCQueue {
public:
    SPSCQueue() noexcept : head_(0), tail_(0) {}

    // 不可拷贝、不可移动（内存地址需固定以保证 cache 对齐）
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    /// 尝试入队一个元素（生产者侧）。
    /// @return 成功返回 true，队列已满返回 false。
    template <typename... Args>
    [[nodiscard]] bool try_push(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<T, Args...>)
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next_tail = (tail + 1) & kMask;

        // next_tail == head 说明队列已满
        if (next_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        // 原地构造元素。
        // slot_ptr() 返回偏移 tail*sizeof(T) 字节的 std::byte* 指针；
        // std::byte* 的指针算术以字节为粒度，语义明确合法。
        new (slot_ptr(tail)) T(std::forward<Args>(args)...);

        // 发布：让消费者可见新元素
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /// 尝试出队一个元素（消费者侧）。
    /// @return 有元素时返回元素值，队列为空返回 std::nullopt。
    [[nodiscard]] std::optional<T> try_pop() noexcept(
        std::is_nothrow_move_constructible_v<T>)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);

        // head == tail 说明队列为空
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }

        // 将 slot[head] 处的原始字节解释为已构造的 T 对象。
        // placement new 之后需要 std::launder 告知编译器该内存已有活跃对象。
        T* elem = std::launder(reinterpret_cast<T*>(slot_ptr(head)));
        std::optional<T> result(std::move(*elem));
        elem->~T();

        // 推进 head：通知生产者该槽位已释放
        head_.store((head + 1) & kMask, std::memory_order_release);
        return result;
    }

    /// 判断队列是否为空（近似值，仅供诊断）。
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    /// 近似队列大小（仅供诊断，非线程安全）。
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const auto h = head_.load(std::memory_order_relaxed);
        const auto t = tail_.load(std::memory_order_relaxed);
        return (t - h) & kMask;
    }

    /// 队列最大容量（实际可用槽位数）。
    static constexpr std::size_t capacity() noexcept { return Cap - 1; }

private:
    static constexpr std::size_t kMask = Cap - 1;

    /// 返回第 idx 个槽位首字节的指针。
    std::byte* slot_ptr(std::size_t idx) noexcept {
        return storage_ + idx * sizeof(T);
    }
    const std::byte* slot_ptr(std::size_t idx) const noexcept {
        return storage_ + idx * sizeof(T);
    }

    // head/tail 各占一个 cache line，防止伪共享
    alignas(kCacheLineSize) std::atomic<std::size_t> head_;
    char pad1_[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    alignas(kCacheLineSize) std::atomic<std::size_t> tail_;
    char pad2_[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    // 原始存储：cache-line 对齐 + 元素对齐的字节数组。
    // 使用 std::byte[] 避免 C++23 废弃的 std::aligned_storage_t<>，
    // sizeof 精确无隐藏舍入。
    alignas(kCacheLineSize) alignas(T) std::byte storage_[sizeof(T) * Cap];
};

}  // namespace minitrader
