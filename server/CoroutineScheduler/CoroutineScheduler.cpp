#include "CoroutineScheduler.h"

void CoroutineScheduler::add(Handle h)
{
    readyQueue.push(h);
}

void CoroutineScheduler::suspend(int fd, uint32_t event, Handle h)
{
    waiting[fd]={event,h};
}

void CoroutineScheduler::resume(int fd, uint32_t event)
{
    auto it=waiting.find(fd);
    if(it==waiting.end())return;

    readyQueue.push(it->second.handle);
    waiting.erase(it);
}

void CoroutineScheduler::runReady()
{
    while (!readyQueue.empty())
    {
        auto h=readyQueue.front();
        readyQueue.pop();
        if(!h.done())
        {
            h.resume();
        }
    }
    
}
