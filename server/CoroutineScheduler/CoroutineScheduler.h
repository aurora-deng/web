#pragma once
#ifndef COROUTINE_SCHEDUER_H
#define COROUTINE_SCHEDUER_H
#include <coroutine>
#include <cstdint>
#include <queue>
#include <unordered_map>
#include <queue>
class CoroutineScheduler
{
public:
    using Handle = std::coroutine_handle<>;
    void add(Handle h);                             // 创建协程
    void suspend(int fd, uint32_t event, Handle h); // 等待协程
    void resume(int fd, uint32_t event);            // 放入到queue中等待进行统一恢复协程
    void runReady();

private:
    std::queue<Handle> ready;
    struct WaitNode
    {
        uint32_t event;
        Handle handle;
    };
    std::queue<Handle> readyQueue;
    std::unordered_map<int, WaitNode> waiting;
};

#endif