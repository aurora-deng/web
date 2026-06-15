#include "TimeWheel.h"


TimerWheel::TimerWheel(size_t slotNum, int timeout):timeout(timeout),slotNum(slotNum)
{
    wheel.resize(slotNum);          //根据时间槽设置时间轮
}

void TimerWheel::add(int fd)
{
    // 修复：如果 fd 已存在，先从旧槽位移除
    auto it = fdPos.find(fd);
    if(it != fdPos.end())
    {
        size_t oldSlot = it->second;
        wheel[oldSlot].remove(fd);
    }

    size_t slot = (current + timeout) % wheel.size();
    wheel[slot].push_back(fd);
    fdPos[fd] = slot;
}

void TimerWheel::remove(int fd)
{
    auto it = fdPos.find(fd);
    if(it == fdPos.end()) return;

    size_t slot = it->second;
    wheel[slot].remove(fd);
    fdPos.erase(it);
}

void TimerWheel::refresh(int fd) {
    add(fd);
}
// 超时处理
void TimerWheel::tick() {
    current=(current+1)%wheel.size();

    auto& slot=wheel[current];                      //获得timernode结点
    for(int fd:slot)                            //遍历删除节时间槽里面的节点
    {
        
        fdPos.erase(fd);
        if(closeCb)
        {
            closeCb(fd);
        }
    }
    slot.clear();
}
void TimerWheel::setCloseCallbace(std::function<void(int)> cb) {
    closeCb=std::move(cb);
};
