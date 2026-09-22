#pragma once

#include "server/CoroutineScheduler/Task.h"

#include <optional>
#include <utility>

// 一条 HTTP/2 业务 stream 的协程所有权与状态机。
//
// 可以把它看成一张可销毁的“叫号牌”：
// - Ready：刚领到牌，还没有进入业务协程；
// - Suspended：Handler 已交给 Worker，协程挂起等待完成通知；
// - Succeeded/Failed：Reactor 恢复协程后，响应提交成功/失败；
// - Cancelled：收到 RST_STREAM 后直接销毁挂起帧，迟到 Worker 结果不再恢复它。
//
// 该对象只允许所属 Reactor 线程操作。Worker 只把 Job 放进完成队列，不能 resume。
class Http2StreamCoroutine
{
public:
    enum class State
    {
        Empty,
        Ready,
        Suspended,
        Succeeded,
        Failed,
        Cancelled
    };

    Http2StreamCoroutine() = default;
    Http2StreamCoroutine(const Http2StreamCoroutine &) = delete;
    Http2StreamCoroutine &operator=(const Http2StreamCoroutine &) = delete;

    bool start(Task<bool> task)
    {
        if (task_ || state_ != State::Empty)
            return false;
        task_.emplace(std::move(task));
        state_ = State::Ready;
        return true;
    }

    State resume()
    {
        if (!task_ || (state_ != State::Ready && state_ != State::Suspended))
            return state_;
        task_->resume();
        if (!task_->done())
        {
            state_ = State::Suspended;
            return state_;
        }

        state_ = task_->result() ? State::Succeeded : State::Failed;
        // Task 停在 final_suspend；这里销毁协程帧，避免把完成帧留到 Job 析构。
        task_.reset();
        return state_;
    }

    void cancel() noexcept
    {
        if (state_ == State::Ready || state_ == State::Suspended)
        {
            task_.reset();
            state_ = State::Cancelled;
        }
    }

    State state() const noexcept { return state_; }
    bool active() const noexcept
    {
        return state_ == State::Ready || state_ == State::Suspended;
    }

private:
    std::optional<Task<bool>> task_;
    State state_ = State::Empty;
};
