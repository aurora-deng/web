#pragma once
#ifndef TASK_H
#define TASK_H
#include <coroutine>
#include <exception>
#include <utility>
// 思路：理解协程创建任务的流程
/*
首先创建对应的结构体变量--->之后调用get_return_object，返回Task结构体
--->之后task构造函数实现-->之后实现initial_suspend
--->return_value--->final_suspend--->返回值--->析构函数结束task
*/
// 协程 Task 包装器。
// Task 在 release() 前拥有协程帧；release() 后所有权必须立即交给
// CoroutineScheduler::adopt()。协程在 final_suspend 停住，调度器清理所有
// 观察引用后通过唯一的 reap 路径销毁帧。
template <typename T = void>
class Task;

template <>
class Task<void>
{
public:
    struct promise_type
    {
        // T value;

        // 获得协程信息
        Task get_return_object()
        {
            return Task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // suspend_always表示先不要启动协程，处于悬挂等待使用resume唤醒
        // suspend_nerver表示先先启动协程，处于启动等待使用co_await暂停之后在使用resume唤醒
        std::suspend_always initial_suspend()
        {
            return {};
        }
        // 表示协程结束后不要立即结束协程，所以使用always
        std::suspend_always final_suspend() noexcept
        {
            return {};
        }

        // 对应co_return
        void return_void() {}

        // 异常处理
        void unhandled_exception()
        {
            std::terminate();
        }

        // void return_value(T v)
        // {
        //     value=v;
        // }
    };
    using Handle = std::coroutine_handle<promise_type>;
    Task(Handle h) : handle(h) {}

    Task(Task &&other) noexcept
        : handle(std::exchange(other.handle, nullptr))
    {
    }

    Task &operator=(Task &&other) noexcept
    {
        if (this != &other)
        {
            if (handle)
                handle.destroy();
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;
    ~Task()
    {
        if (handle)
        {
            handle.destroy();
        }
    }
    void resume()
    {
        if (handle && !handle.done())
            handle.resume();
    }
    bool done() const
    {
        return handle.done();
    }
    Handle release()
    {
        auto h = handle;
        handle = nullptr;
        return h;
    }
    // T result()
    // {
    //     return handle.promise().value;
    // }

private:
    Handle handle;
};

template <typename T>
class Task
{
public:
    struct promise_type
    {
        T value;
        // std::coroutine_handle<> continuation = nullptr;
        // 获得协程信息
        Task get_return_object()
        {
            return Task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // suspend_always表示先不要启动协程，处于悬挂等待使用resume唤醒
        // suspend_nerver表示先先启动协程，处于启动等待使用co_await暂停之后在使用resume唤醒

        std::suspend_always initial_suspend()
        {
            return {};
        }
        // 表示协程结束后不要立即结束协程，所以使用always
         std::suspend_always final_suspend() noexcept
        {
            return {};
        }

        // 表示协程结束后不要立即结束协程，所以使用always
        // auto final_suspend() noexcept
        // {
        //     struct Awaiter
        //     {
        //         bool await_ready() noexcept
        //         {
        //             return false;
        //         }

        //         void await_suspend(std::coroutine_handle<promise_type> h)
        //         {
        //             auto &promise = h.promise();
        //             if (promise.continuation)
        //             {
        //                 promise.continuation.resume();
        //             }
        //         }
        //         void await_resume() {}
        //     };

        //     return Awaiter{};
        // }

        // 对应co_return
        // void return_void() {}

        // 异常处理
        void unhandled_exception()
        {
            std::terminate();
        }

        // 对应co_return
        void return_value(T v)
        {
            value = v;
        }
    };

    using Handle = std::coroutine_handle<promise_type>;
    Task(Handle h) : handle(h) {}

    Task(Task &&other) noexcept
        : handle(std::exchange(other.handle, nullptr))
    {
    }

    Task &operator=(Task &&other) noexcept
    {
        if (this != &other)
        {
            if (handle)
                handle.destroy();
            handle = std::exchange(other.handle, nullptr);
        }
        return *this;
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    ~Task()
    {
        if (handle)
        {
            handle.destroy();
        }
    }
    void resume()
    {
        if (handle && !handle.done())
            handle.resume();
    }
    bool done() const
    {
        return handle.done();
    }
    Handle release()
    {
        auto h = handle;
        handle = nullptr;
        return h;
    }
    T result()
    {
        return std::move(handle.promise().value);
    }
    // bool await_ready()
    // {
    //     return handle.done();
    // }

    // void await_suspend(
    //     std::coroutine_handle<> parent)
    // {
    //     handle.promise().continuation = parent;

    //     handle.resume();
    // }

    // T await_resume()
    // {
    //     return std::move(handle.promise().value);
    // }

private:
    Handle handle;
};

#endif