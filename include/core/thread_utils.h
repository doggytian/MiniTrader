#pragma once

/// @file thread_utils.h
/// 跨平台 CPU 核心绑定 / 调度优化工具。
///
/// 平台差异说明：
///   Linux: sched_setaffinity / pthread_setaffinity_np
///            → 强制绑定，线程只在指定核上运行，效果确定。
///
///   macOS  : THREAD_AFFINITY_POLICY（core_id 作为 affinity tag）
///            → 亲和性"提示"而非强制，M1 的 P/E 核由系统 QoS 管理，
///              Hypervisor/沙盒环境下thread_policy_set 返回 KERN_NOT_SUPPORTED(46)。
///            回退：THREAD_EXTENDED_POLICY(timeshare=0) 请求"固定模式"，
///              使线程脱离 timeshare 调度，降低被换出概率（不指定核，但减少迁移）。
///
///   其他   : 静默no-op，不影响功能。
///
/// 返回值：
///   true= 成功（或平台不支持时no-op 也视为成功）
///   false = 系统调用明确失败（有错误码）

#include <cstdio>
#include <pthread.h>

#ifdef __linux__
#  include <sched.h>
#elif defined(__APPLE__)
#  include <mach/mach.h>
#  include <mach/thread_policy.h>
#endif

namespace minitrader {

/// 将指定线程绑定（或亲和）到某个 CPU 核心。
/// @param handlepthread 句柄（`pthread_self()` 或 `std::thread::native_handle()`）
/// @param core_id 目标核心编号（从 0 开始）；-1 = 不绑定
/// @return 是否成功（macOS no-op 也返回 true）
inline bool pin_thread_to_core(pthread_t handle, int core_id) {
    if (core_id < 0) return true;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<size_t>(core_id), &cpuset);
    int ret = ::pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpuset);
    if (ret != 0) {
        std::fprintf(stderr, "[thread_utils] pthread_setaffinity_np(core=%d) 失败: errno=%d\n",
                     core_id, ret);
        return false;
    }
    return true;

#elif defined(__APPLE__)
    mach_port_t mach_thread = ::pthread_mach_thread_np(handle);

    // 第一步：尝试 THREAD_AFFINITY_POLICY（指定 affinity tag）
    // 在真实 macOS 硬件（非虚拟机/沙盒）上，同tag 的线程会被调度器
    // 尽量放在同一物理核心组；不同tag 的线程则尽量分散。
    // M1 在受限环境下此调用可能返回 KERN_NOT_SUPPORTED(46)，属正常现象。
    {
        thread_affinity_policy_data_t policy = { core_id + 1 };  // tag 0 = 清除，故+1
        kern_return_t kr = ::thread_policy_set(
            mach_thread,
            THREAD_AFFINITY_POLICY,
            reinterpret_cast<thread_policy_t>(&policy),
            THREAD_AFFINITY_POLICY_COUNT);
        if (kr == KERN_SUCCESS) return true;
        // KERN_NOT_SUPPORTED(46)：平台不支持，降级到下一步
    }

    // 第二步：THREAD_EXTENDED_POLICY(timeshare=0)
    // timeshare=FALSE 表示"固定模式"——调度器不再把此线程纳入 timeshare 队列，
    // 降低非自愿上下文切换概率（macOS M1 可接受的最优替代）。
    {
        thread_extended_policy_data_t ext_policy = { FALSE };
        kern_return_t kr = ::thread_policy_set(
            mach_thread,
            THREAD_EXTENDED_POLICY,
            reinterpret_cast<thread_policy_t>(&ext_policy),
            THREAD_EXTENDED_POLICY_COUNT);
        if (kr != KERN_SUCCESS) {
            std::fprintf(stderr,
                "[thread_utils] THREAD_EXTENDED_POLICY 失败: %d（继续运行，绑核无效）\n", kr);
            return false;
        }
    }
    return true;

#else
    (void)handle;
    (void)core_id;
    return true;  // 不支持的平台：静默 no-op
#endif
}

/// 将当前线程绑定到指定核心（封装 pthread_self()）。
inline bool pin_current_thread_to_core(int core_id) {
    return pin_thread_to_core(pthread_self(), core_id);
}

/// 打印绑核结果（方便启动日志确认）。
inline void log_pin_result(const char* thread_name, int core_id, bool ok) {
#ifdef __linux__
    const char* platform = "Linux/强制绑核";
#elif defined(__APPLE__)
    const char* platform = "macOS/亲和性提示";
#else
    const char* platform = "不支持(no-op)";
#endif
    std::printf("[pin] %-16s → core %-2d  %s  [%s]\n",
                thread_name, core_id, ok ? "OK" : "FAIL", platform);
}

}  // namespace minitrader
