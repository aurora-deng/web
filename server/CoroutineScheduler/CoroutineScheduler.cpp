#include "CoroutineScheduler.h"

void CoroutineScheduler::add(Handle h)
{
    if(!h)
        return;

    if(!scheduled.insert(h.address()).second)return;

    readyQueue.push(h);
}

void CoroutineScheduler::suspend(int fd, uint32_t event, Handle h)
{
    waiting[fd] = {event, AwaitType::NONE,h};
}

void CoroutineScheduler::resume(int fd, uint32_t event)
{
    auto it = waiting.find(fd);
    if (it == waiting.end())
        return;
    if (it->second.event != event)
        return;
    auto h=it->second.handle;
    waiting.erase(it);
    add(h);
}

// 协程安全修复：取消 fd 关联的等待协程
void CoroutineScheduler::cancel(int fd)
{
    waiting.erase(fd);
}

void CoroutineScheduler::runReady()
{
    while (!readyQueue.empty())
    {
        auto h = readyQueue.front();
        readyQueue.pop();
        if(!h)continue;
        scheduled.erase(h.address());
        if (h.done()) {
            h.destroy();
        }
        h.resume();

    }
}
