#include "TimeWheel.h"

TimerWheel::TimerWheel(size_t n, TimeoutCB cb):wheel(n),closeCB(cb)
{
}
void TimerWheel::add(int fd,int ttl) {
    auto it=fdPos.find(fd);
    if(it==fdPos.end())return;
    size_t slot=(current+ttl)%wheel.size();
    
    wheel[slot].insert(fd);

    fdPos[fd]=slot;                     //记录fd
}
void TimerWheel::remove(int fd)
{
    auto it=fdPos.find(fd);
    if(it==fdPos.end())return;

    wheel[it->second].erase(fd);
    fdPos.erase(fd);
}
void TimerWheel::refresh(int fd, int ttl) {
    auto it=fdPos.find(fd);
    if(it==fdPos.end())return;
    remove(fd);
    add(fd,ttl);
}
// 超时处理
void TimerWheel::tick() {
    current=(current+1)%wheel.size();

    auto dead=std::move(wheel[current]);

    wheel[current].clear();
    for(int fd:dead)
    {
        fdPos.erase(fd);
        closeCB(fd);
    }
};
