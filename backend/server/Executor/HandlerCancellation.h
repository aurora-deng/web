#pragma once
#ifndef HANDLER_CANCELLATION_H
#define HANDLER_CANCELLATION_H

#include <chrono>
#include <stop_token>

/** handler 主动停止的来源；C++ 不会据此强杀线程。 */
enum class HandlerStopReason
{
    None,
    Cancelled, // 连接关闭或 Runtime 停机主动请求
    Deadline   // 当前单调时钟已越过本次业务截止时间
};

/**
 * @brief 传给业务 handler 的协作式停止预算
 *
 * 像后厨订单上的“撤单灯 + 最晚出餐时间”：业务应在循环、分批 I/O 或重计算边界
 * 调 stopRequested()。它只提供信号，不会抢占线程，也不会自动打断 sleep/阻塞系统调用。
 */
class HandlerCancellation
{
public:
    using Clock = std::chrono::steady_clock;

    HandlerCancellation() = default;
    HandlerCancellation(std::stop_token token, Clock::time_point deadline)
        : token_(token), deadline_(deadline)
    {
    }

    bool cancellationRequested() const noexcept
    {
        return token_.stop_requested();
    }

    bool deadlineExceeded(Clock::time_point now = Clock::now()) const noexcept
    {
        return deadline_ != Clock::time_point::max() && now >= deadline_;
    }

    bool stopRequested(Clock::time_point now = Clock::now()) const noexcept
    {
        return cancellationRequested() || deadlineExceeded(now);
    }

    HandlerStopReason reason(Clock::time_point now = Clock::now()) const noexcept
    {
        if (cancellationRequested())
            return HandlerStopReason::Cancelled;
        if (deadlineExceeded(now))
            return HandlerStopReason::Deadline;
        return HandlerStopReason::None;
    }

    Clock::time_point deadline() const noexcept { return deadline_; }

private:
    std::stop_token token_{};
    Clock::time_point deadline_ = Clock::time_point::max();
};

#endif
