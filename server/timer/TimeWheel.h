#pragma once

#include<vector>
#include<unordered_set>
#include<functional>
#include <cstdint>
#include<unordered_map>
#include <list>
#include"log/logger/logger.h"
#include"server/Buffer/Buffer.h"
// 优化查询时间
struct TimerNode
{
    Conn* conn=nullptr;
    int slot=-1;
    bool removed=false;
};

struct Conn
{
    int fd;
    Buffer readBuffer;
    Buffer writeBuffer;

    HttpResponse resp;
    HttpRequest req;

    TimerNode* timer=nullptr;
    ConnState state;
    size_t sendOffset=0;

    bool useSendfile=false;
};

class TimerWheel{
    public:
    // 超时函数处理

        TimerWheel(size_t slotNum,int timeout);                     //设置槽位数量和超时限制
        void add(Conn& conn);
        void remove(int fd);
        void refresh(Conn& conn);
        // 超时处理
        void tick();
        // 设置关闭函数
        void setCloseCallbace(std::function<void(int)>cb);          
    private:
    // uint64_t current=0;
    // TimeoutCB closeCB;                                             //超时时间函数成员
    // std::vector<std::unordered_set<int>>wheel;                     //时间轮
    // std::unordered_map<int,size_t> fdPos;                          //fd对应的任务位置(即对应时间槽)
    size_t current=0;
    int timeout;                                                       //超时时间设置
    size_t slotNum;                                                 //时间槽
    std::vector<std::list<TimerNode*>>wheel;                        //时间轮

    std::unordered_map<int ,TimerNode*> nodes;

    std::function<void(int)> closeCb;                               //关闭函数
};
