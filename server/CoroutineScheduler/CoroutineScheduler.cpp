// =============================================================================
// 文件名：CoroutineScheduler.cpp
// 职责：实现协程调度器的核心逻辑——析构清理、接收协程、入队调度、运行就绪、回收销毁
//
// 【生活比喻】
// 本文件是"前厅经理"的日常工作手册：酒店打烊时把所有在岗服务员清退（析构）；
// 新员工入职（adopt）；员工被事件召回前排进白板（schedule）；
// 经理按白板顺序叫号上岗（runReady）；员工干完活销更衣柜（reap）。
// 所有协程的"销毁"都必须经过 reap 这一道手续，保证不会 double-free 或漏 free。
//
// 关键技术点（初学者重点理解）：
// 1. 析构时必须 destroy 所有未完成的协程帧，否则协程帧内存会泄漏
//    （C++ 协程帧是堆分配的，必须显式 destroy 才能释放）。
// 2. adopt 使用 emplace + move owner：把外部 shared_ptr 转移进 owned 表，
//    既减少引用计数抖动，又保证 owner 生命周期与协程帧绑定。
// 3. schedule 的去重逻辑：scheduled 集合记录"已在队列但还没跑"的句柄，
//    防止同一协程在等待期间被多次唤醒导致队列里堆积重复项。
// 4. runReady 中 resume 之后立即检查 done()：协程可能 co_return 了，
//    此时必须立即 reap 释放资源；若只是 co_await 挂起，则保留在 owned 表里等下次唤醒。
// =============================================================================
#include "CoroutineScheduler.h"
#include<utility>

/**
 * @brief 析构函数：调度器销毁时清理所有仍持有的协程
 *
 * 【通俗解释】
 * 酒店打烊了：清空待召回白板（readyQueue={}）、清空去重集合（scheduled.clear()），
 * 然后挨个把仍在岗的服务员更衣柜销毁（destroy）。注意要先清空队列再 destroy，
 * 否则队列里残留的句柄会成为悬空指针。
 */
CoroutineScheduler::~CoroutineScheduler()
{
    // 清空就绪队列，避免后续误用已 destroy 的句柄
    readyQueue={};
    // 清空去重集合
    scheduled.clear();
    // 遍历所有仍持有的协程帧，逐个 destroy 释放堆内存
    for(auto &[_,coroutine]:owned)
    {
        if(coroutine.handle)
            coroutine.handle.destroy();
    }
}

/**
 * @brief 接收外部协程帧所有权并立即入队
 * @param fd 关联的 socket fd
 * @ h 协程句柄
 * @param owner session 的 shared_ptr（保证协程存活期间 session 不析构）
 *
 * 【通俗解释】
 * 新服务员入职：①空句柄直接拒收（防御）；②把工号+宿舍床位登记进花名册
 * （emplace + move owner）；③若登记成功（首次入职），立即让他排进待召回队列
 * （schedule）开始第一次出勤。owner 用 move 转移而非拷贝，省一次引用计数原子操作。
 */
void CoroutineScheduler::adopt(int fd,
                               uint64_t connId,
                               CoroutineRole role,
                               Handle h,
                               std::shared_ptr<void> owner)
{
    // 防御性检查：空句柄没意义，直接返回
    if(!h)
        return;
    // emplace 返回 (iterator, inserted)；inserted=false 表示该句柄已存在（重复入职）
    auto [_,inserted] = owned.emplace(
        h.address(), OwnedCoroutine{h, fd, connId, role, std::move(owner)});
    if(inserted)
        schedule(h); // 新登记成功的协程立即入队等待首次 resume
}

// 类似add
/**
 * @brief 将已持有的协程加入就绪队列
 * @param h 协程句柄
 *
 * 【通俗解释】
 * 把服务员工号写到白板上等叫号。三道防线：
 * ①空句柄或不在花名册里的句柄直接拒收（防止外部传入野句柄）；
 * ②scheduled 集合去重——已经在白板上的不再重复写；
 * ③真正入队 push。
 * 这样保证队列里同一协程最多出现一次，避免重复 resume。
 */
void CoroutineScheduler::schedule(Handle h)
{
    // 防御：空句柄或非本调度器持有的句柄不处理
    if(!h||owned.find(h.address())==owned.end())
        return;

    // 去重：scheduled 集合的 insert 返回 pair，.second=false 表示已存在，直接返回
    if(!scheduled.insert(h.address()).second)return;

    // 真正入队，等 runReady 时按 FIFO 顺序 resume
    readyQueue.push(h);
}

/**
 * @brief 设置协程完成回调
 * @param callback 回调函数对象（用 move 转移所有权，避免拷贝）
 *
 * 【通俗解释】
 * 经理向 HR 部门登记"员工离职通知电话"。每次有协程 co_return 完成时，
 * reap 会拨打这个电话通知 SubReactor 做后续清理（清掉 conns 表里的句柄引用）。
 */
void CoroutineScheduler::setCompletionCallback(CompletionCallback callback)
{
    completionCallback=std::move(callback);
}

/**
 * @brief 运行就绪队列：挨个 resume 直到队列空
 *
 * 【通俗解释】
 * 经理开始叫号：从白板头部取一个工号 → 检查工号是否还有效（还在花名册里）→
 * 从去重集合移除（即将运行，下次可再入队）→ 若协程未完成则 resume 让它跑一段 →
 * 跑完后若已完成（done），则调用 reap 回收。注意 resume 过程中协程可能再次
 * 把自己 schedule 进来（递归入队），所以循环条件是 !readyQueue.empty()。
 */
void CoroutineScheduler::runReady()
{
    while (!readyQueue.empty())
    {
        auto h = readyQueue.front(); // 取队首
        readyQueue.pop();            // 出队
        if(!h)continue;              // 防御：空句柄跳过
        // 二次校验：协程可能已被 reap 销毁，不在花名册里则跳过
        if(owned.find(h.address())==owned.end())continue;
        // 从去重集合移除：即将运行，下次 schedule 可以再次入队
        scheduled.erase(h.address());
        if (!h.done()) {
            h.resume(); // 真正让协程跑一段，可能跑到 co_await 又挂起，或 co_return 完成
        }
        // resume 后再次检查：若协程已 co_return（done），立即回收资源
        if(h.done())
        {
            reap(h);
        }
    }
}

// 类似删除函数
/**
 * @brief 回收一个已完成（done）的协程：触发回调、销毁协程帧
 * @param h 已 done 的协程句柄
 *
 * 【通俗解释】
 * 服务员离职手续：①空句柄直接返回；②在花名册里找到记录，取出 fd 和 owner；
 * ③从去重集合清除（虽然此时一般已不在）；④拨打完成回调电话通知 SubReactor；
 * ⑤从花名册 erase 记录；⑥销毁协程帧（destroy）。
 * 这是协程帧销毁的唯一入口——任何别的地方都不应直接 destroy，
 * 这样所有销毁路径集中在一处，便于排查 double-free 问题。
 */
void CoroutineScheduler::reap(Handle h)
{
    // 防御：空句柄不处理
    if(!h)
        return;

    // 在花名册里查找该协程记录
    auto it=owned.find(h.address());
    if(it==owned.end())return; // 不在本调度器管辖范围，跳过

    // 取出 fd 和 owner（owner 出作用域后引用计数 -1，可能触发 session 析构）
    const int fd = it->second.fd;
    const uint64_t connId = it->second.connId;
    const CoroutineRole role = it->second.role;
    auto owner=std::move(it->second.owner);
    // 清除去重集合中的残留（通常已在 runReady 里 erase 过，这里兜底）
    scheduled.erase(h.address());
    // 拨打完成回调：SubReactor 借此清掉 conns[fd] 里的 handle 引用
    if(completionCallback)
        completionCallback(fd, connId, role, h);
    // 从花名册 erase 记录
    owned.erase(it);
    // 最终销毁协程帧，释放堆内存
    h.destroy();
}
