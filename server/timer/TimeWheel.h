#ifndef TIME_WHEEL_H
#define TIME_WHEEL_H

#include<vector>
#include<unordered_set>
#include<functional>
#include <cstdint>
#include<unordered_map>
#include <list>
#include <utility>

#include"log/logger/logger.h"
// 优化查询时间


class TimerWheel{
    public:
    // 超时函数处理

        TimerWheel(size_t slotNum,int timeout);                     //设置槽位数量和超时限制
        void add(int fd);
        void remove(int fd);
        void refresh(int fd);
        // 超时处理
        void tick();
        // 设置关闭函数
        void setCloseCallbace(std::function<void(int)>cb);          
    private:
    std::unordered_map<int,size_t> fdPos;                          //fd对应的任务位置(即对应时间槽)
    size_t current=0;
    int timeout;                                                       //超时时间设置
    size_t slotNum;                                                 //时间槽
    std::vector<std::list<int>>wheel;                        //时间轮


    std::function<void(int)> closeCb;                               //关闭函数
};

#endif