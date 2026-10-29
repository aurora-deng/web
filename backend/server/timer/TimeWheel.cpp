// ============================================================
// 文件：TimeWheel.cpp
// 职责：时间轮超时管理器的具体实现
//
// 【生活比喻：酒店巡查员的工作手册】
//   TimeWheel.h 是"岗位说明"，这个 .cpp 就是巡查员的具体工作手册：
//   怎么给客人登记（add）、怎么注销客人（remove）、怎么给客人续期（refresh）、
//   每秒走一格时怎么处理超时客人（tick）。巡查员自己不关门（不直接 close），
//   而是对讲机通知前台（closeCb 回调）由前台处理。
//
// 关键技术点（初学者重点理解）：
//   1. add 的"先删旧再添新"：防止同一个 fd 出现在两个格子里导致重复关闭。
//   2. 取模运算：(current + timeout) % wheel.size() 实现环形轮盘效果。
//   3. tick 的清空操作：超时格子的 fd 全部处理完后 slot.clear()，无需逐个 remove。
//   4. closeCb 回调解耦：TimeWheel 不依赖 SubReactor 的具体类型，只通过回调通知。
// ============================================================
#include "TimeWheel.h"


/**
 * @brief 构造时间轮：初始化轮盘大小
 * @param slotNum 格子总数（轮盘转一圈的刻度数）
 * @param timeout 超时秒数（每 tick 一次代表 1 秒）
 *
 * 【准备挂钟】构造时把轮盘的 N 个格子都建好（空 list），指针 current 从 0 开始。
 */
TimerWheel::TimerWheel(size_t slotNum, int timeout):timeout(timeout),slotNum(slotNum)
{
    wheel.resize(slotNum);          // 根据格子数量初始化轮盘，每个格子是一个空 list<int>
}

/**
 * @brief 登记/续期一个 fd
 * @param fd 待登记的文件描述符
 *
 * 【给客人登记倒计时】
 *   把 fd 放到"当前指针 + 超时秒数"对应的格子里，到期时指针走到就超时。
 *   如果 fd 之前登记过（续期场景），必须先从旧格子移除，否则同一 fd 会
 *   同时在两个格子里，导致旧格子到期时误关一个还活跃的连接。
 */
void TimerWheel::add(int fd)
{
    // 修复：如果 fd 已存在，先从旧槽位移除，避免重复登记导致误关闭
    auto it = fdPos.find(fd);
    if(it != fdPos.end())
    {
        size_t oldSlot = it->second;
        wheel[oldSlot].remove(fd);  // 从旧格子摘除
    }

    // 计算目标格子：(当前指针 + 超时) 对轮盘大小取模 → 环形轮盘效果
    size_t slot = (current + timeout) % wheel.size();
    wheel[slot].push_back(fd);      // 放进目标格子
    fdPos[fd] = slot;               // 更新速查表，记下 fd 现在在哪个格子
}

/**
 * @brief 主动移除一个 fd（连接正常关闭时调用）
 * @param fd 待移除的文件描述符
 *
 * 【注销客人】连接关闭时把 fd 从时间轮里摘掉，防止关闭后还被 tick 触发 closeCb。
 */
void TimerWheel::remove(int fd)
{
    auto it = fdPos.find(fd);
    if(it == fdPos.end()) return;   // fd 不在时间轮里，直接返回（防御性检查）

    size_t slot = it->second;       // 查出在哪个格子
    wheel[slot].remove(fd);         // 从格子的 list 中摘除
    fdPos.erase(it);                // 从速查表中删除
}

/**
 * @brief 刷新 fd 的超时倒计时（收到数据时调用）
 * @param fd 待刷新的文件描述符
 *
 * 【续期】客人有活动就重新倒计时。直接复用 add()——它内部已经处理了"先删旧再添新"。
 */
void TimerWheel::refresh(int fd) {
    add(fd);  // add 内部会先从旧格子移除再放到新格子，等价于续期
}

/**
 * @brief 推进时间轮一格（每秒调用一次）
 *
 * 【巡查员每小时巡一格】
 *   指针 current 前进一格，走到哪个格子，那个格子里的所有 fd 都已超时。
 *   逐个调用 closeCb 通知 SubReactor 关闭连接，然后清空整个格子。
 *   先从 fdPos 移除再调 closeCb，防止回调中再次操作时间轮导致迭代器失效。
 */
void TimerWheel::tick() {
    // ---- 步骤1：指针前进一格（取模实现环形循环）----
    current=(current+1)%wheel.size();

    // ---- 步骤2：取出当前格子的所有 fd ----
    auto& slot=wheel[current];                      // 当前格子里的 fd 列表
    for(int fd:slot)                            // 遍历超时的 fd
    {
        // 先从速查表移除，再调回调；防止回调内部再次操作时间轮导致迭代器失效
        fdPos.erase(fd);
        if(closeCb)
        {
            closeCb(fd);  // 通知 SubReactor 关闭连接（不是直接 close）
        }
    }
    // ---- 步骤3：清空整个格子（fd 已全部处理完）----
    slot.clear();
}

/**
 * @brief 设置超时关闭回调
 * @param cb 回调函数，参数为超时的 fd
 *
 * 【对讲机频道设置】TimeWheel 通过这个回调通知外部"哪个 fd 超时了"，
 *   具体的关闭逻辑（epoll_ctl DEL、close(fd)、清理 Connection）由 SubReactor 注入。
 *   用 std::move 避免拷贝 std::function 对象。
 */
void TimerWheel::setCloseCallbace(std::function<void(int)> cb) {
    closeCb=std::move(cb);
};
