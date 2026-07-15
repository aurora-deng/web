#pragma once
#ifndef COROUTINE_SCHEDUER_H
#define COROUTINE_SCHEDUER_H
#include <coroutine>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <queue>
#include <algorithm>
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
    void add(Handle h);                             // 创建协程
    void suspend(int fd, uint32_t event, Handle h); // 等待协程
    void resume(int fd, uint32_t event);            // 放入到queue中等待进行统一恢复协程
    void runReady();
    // 协程安全修复：取消 fd 关联的等待协程
    // fd_close 时调用，防止已关闭 fd 的协程被意外唤醒
    void cancel(int fd);

private:
    std::queue<Handle> ready;
    struct WaitNode
    {
        uint32_t event;
        AwaitType type;
        Handle handle;
    };
    std::queue<Handle> readyQueue;
    std::unordered_map<int, WaitNode> waiting;
};

#endif