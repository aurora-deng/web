// =============================================================================
// 文件名：CoroutineSlot.h
// 所属模块：server/CoroutineScheduler —— 协程槽位与角色定义
//
// 【职责比喻：服务员工号牌 / 协程排班表里的格子】
// 本文件定义协程调度体系的最基础数据结构：
//   - AwaitType：协程当前在等什么事件（读/写/执行/出站/发送完成/定时器）
//   - CoroutineRole：协程的角色身份（Main 主协程=服务员 / Writer 写协程=写工）
//   - CoroutineSlot：单个协程的"工号牌格子"，存协程句柄 + 等待状态 + 是否在挂起
//   - CoroutineSlots：一条连接上所有角色槽位的定长数组（按 CoroutineRole 索引）
//   - roleIndex：把 CoroutineRole 枚举转成数组下标的工具函数
// 一条连接同时有 Main 和 Writer 两个协程在跑（互不阻塞），它们各自的句柄/状态分别
// 存在 CoroutineSlots[Main] 和 CoroutineSlots[Writer] 两个槽位里——这是 2.0 协程
// 调度编排的核心数据结构。
//
// 关键技术点（初学者重点理解）：
// 1. 【Main/Writer 双协程】每条连接拉起两条协程：Main 跑 Session::run()（解析请求→
//    业务→发响应），Writer 跑 writerLoop（持续冲刷出站队列）。读写分离避免发送
//    阻塞卡住请求解析路径。CoroutineRole::Count=2 是槽位数组的固定长度。
// 2. 【AwaitType 状态机】state 字段记录协程当前在等什么事件：READ/WRITE 等 epoll、
//    EXECUTE 等后厨、OUTBOUND 等队列非空、SENT 等发送完成、TIMER 等定时器。
//    wakeCoroutine(fd, role, expected) 通过比 state==expected 决定是否唤醒，
//    避免错误唤醒（如协程已切换到别的等待状态时还按旧状态唤醒）。
// 3. 【waiting 标记】waiting=true 表示协程当前挂在某个 Awaiter 上；恢复后置 false。
//    主要用于诊断与 Awaiter::await_resume 的收尾清理，与 state 配合描述协程生命周期。
// 4. 【handle 默认为 nullptr】协程尚未 adopt 或已 done 时 handle 为空，使用前必须判空，
//    否则 resume 空句柄是未定义行为。
// =============================================================================
#pragma once
#ifndef COROUTINE_SLOT_H
#define COROUTINE_SLOT_H

#include <array>
#include <coroutine>
#include <cstddef>

/**
 * @brief 协程等待事件类型
 *
 * 【通俗解释】
 * 协程当前挂在哪种 Awaiter 上等事件，对应六种叫号牌加一个"无等待"初值：
 *   NONE     - 无等待（协程刚 adopt 或刚被唤醒，未挂到任何 Awaiter 上）
 *   READ     - 等 socket 可读（ReadAwaiter）
 *   WRITE    - 等 socket 可写（WriteAwaiter / TransportWriteAwaiter）
 *   EXECUTE  - 等业务 handler 在 Executor 跑完（ExecuteAwaiter）
 *   OUTBOUND - 等出站队列从空变非空（OutboundAwaiter，仅 Writer 协程会用）
 *   SENT     - 等 ticket 对应出站任务发完（SendCompletionAwaiter）
 *   TIMER    - 等定时器触发
 */
enum class AwaitType
{
    NONE,
    READ,
    WRITE,
    EXECUTE,
    OUTBOUND,
    SENT,
    TIMER
};

/**
 * @brief 协程角色
 *
 * 【通俗解释】
 * 每条连接同时跑两条协程，各自有不同身份：
 *   Main   - 主协程（服务员）：跑 Session::run()，负责解析请求→分发业务→发响应；
 *   Writer - 写协程（写工）：跑 writerLoop，持续冲刷出站队列把数据 write 到 socket；
 *   Count  - 角色总数（=2），用作 CoroutineSlots 数组的长度。
 * 读写分离让发送阻塞不会卡住请求解析。两个角色的句柄/状态分别存在槽位数组的
 * [Main] 和 [Writer] 位置，互不串扰。
 */
enum class CoroutineRole : std::size_t
{
    Main = 0,
    Writer = 1,
    Count = 2
};

/**
 * @brief 单个协程的槽位（工号牌格子）
 *
 * 【通俗解释】
 * 一个槽位存一条协程的全部调度信息：
 *   handle  - 协程句柄（resume/done 测试的凭证），未 adopt 或已 done 时为 nullptr；
 *   state   - 当前在等什么事件（AwaitType），wakeCoroutine 凭此判断是否该唤醒；
 *   waiting - 是否当前挂在 Awaiter 上挂起中，恢复后置 false。
 */
struct CoroutineSlot
{
    std::coroutine_handle<> handle{};        // 协程句柄：默认空，adopt 后赋值，fd_close 时清空
    AwaitType state = AwaitType::NONE;       // 等待事件类型：默认 NONE，挂起时由 Awaiter 设为对应类型
    bool waiting = false;                    // 是否挂起等待中：true=挂在 Awaiter 上；恢复后置 false
};

/**
 * @brief 一条连接上所有角色槽位的定长数组
 *
 * 【通俗解释】
 * 每条 Connection 内嵌一个 CoroutineSlots，长度固定为 CoroutineRole::Count（=2）：
 *   slots[Main]   - 服务员（主协程）的工号牌格子
 *   slots[Writer] - 写工（写协程）的工号牌格子
 * 通过 slot(role) 取对应角色的格子，避免裸数组下标写错。
 */
using CoroutineSlots =
    std::array<CoroutineSlot, static_cast<std::size_t>(CoroutineRole::Count)>;

/**
 * @brief 把 CoroutineRole 枚举转成数组下标
 * @param role 协程角色
 * @return 对应的 std::size_t 下标（0=Main, 1=Writer）
 *
 * 【通俗解释】
 * CoroutineRole 本身是强类型枚举，不能直接当数组下标用——这个 inline 函数把它
 * 显式转成 size_t，让 slots[roleIndex(role)] 这种写法可读又安全。
 */
inline constexpr std::size_t roleIndex(CoroutineRole role)
{
    return static_cast<std::size_t>(role);
}

#endif
