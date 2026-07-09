#pragma once
#ifndef TASK_H
#define TASK_H
#include <coroutine>
#include <exception>
class Task
{
public:
    struct promise_type
    {
        Task get_return_object()
        {
            return Task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend()
        {
            return {};
        }

        std::suspend_always final_suspend() noexcept
        {
            return {};
        }

        void return_void() {}

        void unhandled_exception()
        {
            std::terminate();
        }
    };
    public:
    using Handle = std::coroutine_handle<promise_type>;
    explicit Task(Handle h) : handle(h) {}

    Task(Task &&other)
    {
        handle = other.handle;
        other.handle = nullptr;
    }

    ~Task()
    {
        if (handle)
        {
            handle.destroy();
        }
    }
    void resume()
    {
        handle.resume();
    }
    bool done()const
    {
        return handle.done();
    }
    Handle release()
    {
        auto h = handle;
        handle = nullptr;
        return h;
    }

private:
    Handle handle;
};

#endif