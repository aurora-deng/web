#pragma once

#include<vector>
#include<unordered_set>
#include<functional>
#include <cstdint>
#include<unordered_map>

class TimerWheel{
    public:
    // 超时函数处理
        using TimeoutCB=std::function<void(int)>;

        TimerWheel(size_t slotNum,TimeoutCB cb);
        void add(int fd,int ttl);
        void remove(int fd);
        void refresh(int fd,int ttl);
        // 超时处理
        void tick();

    private:
    uint64_t current=0;
    TimeoutCB closeCB;                                             //超时时间函数成员
    std::vector<std::unordered_set<int>>wheel;                     //时间轮
    std::unordered_map<int,size_t> fdPos;                          //fd对应的任务位置(即对应时间槽)
};
