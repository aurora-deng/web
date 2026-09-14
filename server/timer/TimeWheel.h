// ============================================================
// 文件：TimeWheel.h
// 职责：连接超时管理——用"时间轮"高效踢掉空闲连接
//
// 【生活比喻：酒店大堂的挂钟 + 巡查员】
//   想象酒店大堂墙上挂着一个只有 N 个刻度的钟（时间轮 wheel），指针每秒
//   往前走一格（tick）。每位入住客人（fd）登记时，巡查员在"当前时间 + 超时秒数"
//   对应的刻度格子里记下客人编号。指针每走到一个格子，就检查这个格子里
//   的客人——如果他们一直没活动（没 refresh 刷新），说明已经超时，巡查员
//   就把他们的房卡注销（调用 closeCb 关闭连接）。
//   客人只要有活动（收到数据），前台就把他们的名字从旧格子挪到新格子，
//   重新开始倒计时。
//
//   【为什么不用遍历所有定时器】
//   如果每秒遍历所有连接检查是否超时，连接多时 O(n) 太慢。时间轮把超时
//   检查变成 O(1)——指针走到哪个格子，那个格子的连接就全部超时，无需遍历。
//
// 关键技术点（初学者重点理解）：
//   1. 时间轮原理：用 vector<list<int>> 表示轮盘，每个 list 是一个"格子"，
//      存放到期时间相同的 fd。tick() 推进指针，当前格子全部超时。
//   2. fdPos 快速定位：unordered_map<fd, slot> 记录每个 fd 在哪个格子，
//      refresh 时能 O(1) 找到旧位置移除，再插入新格子，避免遍历查找。
//   3. 回调式关闭：TimeWheel 不直接 close(fd)，而是通过 closeCb 回调通知
//      SubReactor 执行关闭流程（需更新 epoll、清理 Connection 等）。
//   4. refresh = remove + add：刷新超时只需把 fd 从旧格子挪到新格子，
//      复用 add 内部的"先删旧再添新"逻辑。
// ============================================================
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

/**
 * @brief 时间轮：高效的连接超时管理器
 *
 * 【挂钟 + 巡查员】
 *   用一个 vector<list> 模拟钟盘，指针 current 每秒走一格。fd 登记时
 *   放到"当前+超时"对应的格子，指针走到时全部超时关闭。fdPos 速查表
 *   保证刷新/移除时 O(1) 定位，无需遍历。
 */
class TimerWheel{
    public:
    /**
     * @brief 构造时间轮
     * @param slotNum 轮盘格子总数（决定轮盘大小，通常 >= 超时秒数）
     * @param timeout 超时秒数（fd 登记后经过这么多个 tick 仍无活动则超时）
     */
    TimerWheel(size_t slotNum,int timeout);

    /**
     * @brief 登记/续期一个 fd（放到"当前+超时"对应的格子）
     * @param fd 待登记的文件描述符
     * 若 fd 已存在，先从旧格子移除再放到新格子（相当于续期）。
     */
    void add(int fd);

    /**
     * @brief 主动移除一个 fd（连接关闭时调用，避免残留）
     * @param fd 待移除的文件描述符
     */
    void remove(int fd);

    /**
     * @brief 刷新 fd 的超时倒计时（收到数据时调用，重新计时）
     * @param fd 待刷新的文件描述符
     * 内部实现就是 add(fd)——add 已包含"先删旧再添新"逻辑。
     */
    void refresh(int fd);

    /**
     * @brief 推进时间轮一格（每秒调用一次）
     * 指针 current 前进一格，当前格子里的所有 fd 超时，调用 closeCb 关闭。
     */
    void tick();

    /**
     * @brief 设置超时关闭回调函数
     * @param cb 回调函数，参数为超时的 fd
     * TimeWheel 不直接 close(fd)，而是通过回调通知 SubReactor 执行完整关闭流程。
     */
    void setCloseCallbace(std::function<void(int)>cb);

    private:
    std::unordered_map<int,size_t> fdPos;   // fd → 所在格子编号的速查表（O(1) 定位）
    size_t current=0;                       // 当前指针位置（每 tick 前进一格）
    int timeout;                            // 超时秒数（fd 放到 current+timeout 格子）
    size_t slotNum;                         // 轮盘格子总数
    std::vector<std::list<int>>wheel;       // 时间轮本体：每个 list 存该格子到期的 fd 集合

    std::function<void(int)> closeCb;       // 超时关闭回调（由 SubReactor 注入）
};

#endif