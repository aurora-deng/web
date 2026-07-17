#include "CoroutineScheduler.h"
#include<utility>

CoroutineScheduler::~CoroutineScheduler()
{
    readyQueue={};
    scheduled.clear();
    for(auto &[_,coroutine]:owned)
    {
        if(coroutine.handle)
            coroutine.handle.destroy();
    }
}

void CoroutineScheduler::adopt(int fd, Handle h, std::shared_ptr<void> owner)
{
    if(!h)
        return;
    auto [_,inserted]=owned.emplace(h.address(),OwnedCoroutine{h,fd,std::move(owner)});
    if(inserted)
        schedule(h);
}

// 类似add
void CoroutineScheduler::schedule(Handle h)
{
    if(!h||owned.find(h.address())==owned.end())
        return;

    if(!scheduled.insert(h.address()).second)return;

    readyQueue.push(h);
}

void CoroutineScheduler::setCompletionCallback(CompletionCallback callback)
{
    completionCallback=std::move(callback);
}

void CoroutineScheduler::runReady()
{
    while (!readyQueue.empty())
    {
        auto h = readyQueue.front();
        readyQueue.pop();
        if(!h)continue;
        if(owned.find(h.address())==owned.end())continue;
        scheduled.erase(h.address());
        if (!h.done()) {
            h.resume();
        }
        if(h.done())
        {
            reap(h);
        }
    }
}

// 类似删除函数
void CoroutineScheduler::reap(Handle h)
{
    if(!h)
        return;
    
    auto it=owned.find(h.address());
    if(it==owned.end())return;

    const int fd=it->second.fd;
    auto owner=std::move(it->second.owner);
    scheduled.erase(h.address());
    if(completionCallback)
        completionCallback(fd,h);
    owned.erase(it);
    h.destroy();
}

