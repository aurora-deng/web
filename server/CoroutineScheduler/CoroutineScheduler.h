#pragma once
#ifndef COROUTINE_SCHEDUER_H
#define COROUTINE_SCHEDUER_H
#include <coroutine>
#include<functional>
#include<memory>
#include <queue>
#include <unordered_map>
#include<unordered_set>


enum class AwaitType
{
    NONE,
    READ,
    WRITE,
    TIMER
};
// 用于保存handle
class CoroutineScheduler
{
public:
    using Handle = std::coroutine_handle<>;
    using CompletionCallback=std::function<void(int,Handle)>;
    ~CoroutineScheduler();
    // 接收 Task::release() 转移过来的协程帧所有权；owner 保证成员协程的
    // HttpSession 在协程帧销毁前仍然存活。
    void adopt(int fd, Handle h, std::shared_ptr<void> owner = {});
    // 将已由调度器持有的协程加入就绪队列。
    void schedule(Handle h);
    
    void setCompletionCallback(CompletionCallback callback);
    void runReady();

private:
    struct OwnedCoroutine
    {
        Handle handle;
        int fd;
        std::shared_ptr<void> owner;
    };
    void reap(Handle h);
    std::queue<Handle> readyQueue;
    std::unordered_map<void*,OwnedCoroutine> owned;
    std::unordered_set<void*> scheduled;
    CompletionCallback completionCallback;
};

#endif