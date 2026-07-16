#include "CoroutineScheduler.h"

void CoroutineScheduler::add(Handle h)
{
    if(!h)
        return;

    auto addr=h.address();

    if(scheduled.count(addr))
        return;

    scheduled.insert(addr);

    readyQueue.push(h);
}

void CoroutineScheduler::suspend(int fd, uint32_t event, Handle h)
{
    waiting[fd] = {event, h};
}

void CoroutineScheduler::resume(int fd, uint32_t event)
{
    auto it = waiting.find(fd);
    if (it == waiting.end())
        return;
    if (it->second.event != event)
        return;
    // auto evIt = it->second.find(event);
    // if (evIt == it->second.end())
    //     return;
    // readyQueue.push(evIt->second);
    readyQueue.push(it->second.handle);
    waiting.erase(it);
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
        scheduled.erase(h.address());
        h.resume();
        if(!h)continue;

        if (h.done())
        {
            h.destroy();
            continue;
        }
    }
}
