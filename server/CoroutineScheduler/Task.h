// =============================================================================
// 文件名：Task.h
// 职责：定义 C++20 协程的"任务包装" Task<T>——协程函数的返回类型，
//       封装协程句柄的生命周期管理（resume / done / release / destroy）。
//
// 【生活比喻】
// Task 像是服务员入职时领到的"工牌+更衣柜钥匙"。协程函数（如 HttpSession::run）
// 一被调用，编译器就会在堆上分配一个协程帧（更衣柜），并返回一个 Task（工牌）。
// 拿到工牌的人可以：①resume() 让服务员上岗干活；②done() 看他是不是干完了；
// ③release() 把工牌转交给前厅经理（CoroutineScheduler::adopt），从此工牌归经理管；
// ④析构时如果工牌还在自己手里，就 destroy 更衣柜释放内存。
// 设计要点：协程在 final_suspend 处停住不自动销毁，必须由"持有工牌的人"显式 destroy。
//
// 关键技术点（初学者重点理解）：
// 1. promise_type 是协程的"承诺对象"，编译器为每个协程调用生成一个 promise，
//    通过它控制协程行为（initial_suspend/final_suspend/return_void/return_value 等）。
// 2. initial_suspend 返回 suspend_always：协程创建后立即挂起，不自动跑，
//    让调用方决定何时 resume 第一次（lazy 协程）；若返回 suspend_never 则是 eager。
// 3. final_suspend 返回 suspend_always：协程 co_return 后停在最终挂起点不自动销毁，
//    这样外部持有句柄的人还能查询 done() 状态、安全 destroy。若返回 suspend_never，
//    协程一 return 就自毁，外部句柄立刻变野，极度危险。
// 4. release() 转移所有权：把内部 handle 置空，外部拿到裸 handle 接管 destroy 责任。
//    这是把协程交给 CoroutineScheduler::adopt 的标准姿势。
// 5. 本文件提供 Task<void>（无返回值）和 Task<T>（有返回值）两个特化版本，
//    服务器里 HttpSession::run 返回 Task<void>，SubReactor::writerLoop 返回 Task<void>。
// =============================================================================
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

// =============================================================================
// Task<void> 特化：协程无返回值（co_return; 不带值）
// =============================================================================
template <>
class Task<void>
{
public:
    /**
     * @brief promise_type：协程的"承诺对象"，编译器为每个协程生成一个
     *
     * 【通俗解释】
     * promise 是协程的"内部大脑"：编译器调用它的方法来控制协程生命周期各阶段。
     *   get_return_object - 创建返回给调用方的 Task 对象
     *   initial_suspend   - 协程刚创建时是否立即挂起
     *   final_suspend     - 协程 co_return 后是否挂起（不自动销毁）
     *   return_void       - 处理 co_return;（无值）
     *   unhandled_exception - 协程内未捕获异常时调用
     */
    struct promise_type
    {
        // T value;

        // 获得协程信息
        /**
         * @brief 创建返回给调用方的 Task 对象
         * @return 包含当前协程句柄的 Task
         *
         * 【通俗解释】协程刚创建时调用一次，把"工牌"递给调用方。
         *            from_promise(*this) 通过 promise 反向拿到协程句柄。
         */
        Task get_return_object()
        {
            return Task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // suspend_always表示先不要启动协程，处于悬挂等待使用resume唤醒
        // suspend_nerver表示先先启动协程，处于启动等待使用co_await暂停之后在使用resume唤醒
        /**
         * @brief 协程创建后立即调用，决定是否立即挂起
         * @return suspend_always 表示立即挂起（lazy 协程），等调用方 resume 才开始跑
         *
         * 【通俗解释】服务员入职领工牌后先不干活，等经理叫号才上岗。
         */
        std::suspend_always initial_suspend()
        {
            return {};
        }
        // 表示协程结束后不要立即结束协程，所以使用always
        /**
         * @brief 协程 co_return 后调用，决定是否自动销毁
         * @return suspend_always 表示挂起不销毁（让外部安全 destroy）
         *
         * 【通俗解释】服务员干完活后站在原地不动，等经理来销更衣柜。
         *            若用 suspend_never，协程一完成就自毁，外部句柄立刻变野指针。
         */
        std::suspend_always final_suspend() noexcept
        {
            return {};
        }

        // 对应co_return
        /**
         * @brief 处理 co_return;（无值返回）
         *
         * 【通俗解释】服务员说"我干完了"，啥也没带回来，啥也不做。
         */
        void return_void() {}

        // 异常处理
        /**
         * @brief 协程内未捕获异常时调用
         *
         * 【通俗解释】服务员工作中突发意外没法处理，直接终止整个程序
         *            （生产环境可改为存起异常让调用方查询 rethrow）。
         */
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

    /**
     * @brief 从协程句柄构造 Task
     * @param h 协程句柄
     */
    Task(Handle h) : handle(h) {}

    /**
     * @brief 移动构造：把句柄从另一个 Task 转移过来，原 Task 置空
     *
     * 【通俗解释】工牌转交给别人，自己手里没工牌了（handle=nullptr），
     *            析构时不会误销更衣柜。std::exchange 同时取值并赋空，原子操作。
     */
    Task(Task &&other) noexcept
        : handle(std::exchange(other.handle, nullptr))
    {
    }

    /**
     * @brief 移动赋值：先销毁自己持有的旧句柄，再转移新句柄
     *
     * 【通俗解释】换工牌：先把旧更衣柜销毁（如果有的话），再接手新工牌。
     *            自赋值检查防止把自己销毁掉。
     */
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

    // 禁用拷贝：协程帧是唯一所有权资源，不能两个 Task 同时持有一个句柄
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    /**
     * @brief 析构：若仍持有句柄则销毁协程帧
     *
     * 【通俗解释】工牌丢了（没 release 出去），更衣柜也得跟着销毁，否则内存泄漏。
     */
    ~Task()
    {
        if (handle)
        {
            handle.destroy();
        }
    }

    /**
     * @brief 让协程跑一段（从挂起点恢复执行）
     *
     * 【通俗解释】叫服务员上岗。先检查句柄有效且未完成，再 resume。
     */
    void resume()
    {
        if (handle && !handle.done())
            handle.resume();
    }

    /**
     * @brief 查询协程是否已完成（co_return 过）
     * @return true=已完成；false=仍在挂起或运行中
     */
    bool done() const
    {
        return handle.done();
    }

    /**
     * @brief 放弃句柄所有权，把裸句柄交给调用方
     * @return 裸协程句柄（调用方负责 destroy）
     *
     * 【通俗解释】把工牌正式交给前厅经理（CoroutineScheduler::adopt）。
     *            自己手里置空，析构时不会再 destroy，避免 double-free。
     *            这是把协程交给调度器托管的标准姿势。
     */
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

// =============================================================================
// Task<T> 主模板：协程有返回值（co_return value;）
// =============================================================================
template <typename T>
class Task
{
public:
    /**
     * @brief promise_type：带返回值的协程承诺对象
     *
     * 【通俗解释】
     * 与 Task<void> 的 promise 几乎一样，唯一区别是多了一个 value 成员
     * 和 return_value(T) 方法，用来存 co_return 带回来的值。
     * result() 方法把存好的值 move 出来给调用方。
     */
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
        /**
         * @brief 处理 co_return value;（带值返回）
         * @param v 协程返回的值
         *
         * 【通俗解释】服务员干完活带回一样东西，存在 promise 的 value 里，
         *            调用方通过 result() 取走。
         */
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

    /**
     * @brief 取出协程的返回值
     * @return 协程通过 co_return 返回的值（move 语义转移）
     *
     * 【通俗解释】服务员干完活带回的东西，从 promise 里 move 出来交给调用方。
     *            调用前应先确认 done()==true，否则行为未定义。
     */
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
