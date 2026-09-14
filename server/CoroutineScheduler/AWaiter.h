// =============================================================================
// 文件名：AWaiter.h
// 职责：定义协程的"等待器"（Awaiter）家族——ReadAwaiter/WriteAwaiter/ExecuteAwaiter/
//       OutboundAwaiter/TransportWriteAwaiter/SendCompletionAwaiter
//
// 【生活比喻】
// 把整个服务器想象成一家大酒店：协程是服务员，服务员点完单之后不会傻站在厨房门口
// 等菜，而是去服务别的桌；菜好了再回来端菜。Awaiter 就是那张"叫号牌"——
// 服务员把叫号牌留给厨房（注册等待），自己挂起去干别的事；厨房做好菜后通过叫号牌
// 把服务员召回，服务员继续完成"上菜"这步。本文件就是定义六种不同叫号牌：
//   - ReadAwaiter：等"客人发话"（socket 可读）的叫号牌
//   - WriteAwaiter：等"客人能听"（socket 可写）的叫号牌
//   - ExecuteAwaiter：等"后厨做完菜"（业务 handler 跑完）的叫号牌
//   - OutboundAwaiter：写工等"出站队列有活干"（writerLoop 队列非空）的叫号牌
//   - TransportWriteAwaiter：写工等"客人能听"（socket 可写，EPOLLOUT）的叫号牌
//   - SendCompletionAwaiter：服务员等"我提交的那批菜已发完"（ticket 完成）的叫号牌
//
// 关键技术点（初学者重点理解）：
// 1. C++20 协程 Awaiter 三件套：await_ready/await_suspend/await_resume。
//    协程遇到 co_await expr 时，编译器会调用 expr 的这三个方法决定"是否挂起/怎么挂起/恢复时返回什么"。
// 2. await_suspend 返回 true 表示真正挂起协程并把控制权还给调度器；返回 false 则不挂起直接继续。
// 3. 前向声明 SubReactor 而非直接 include，是为了打破头文件循环依赖（SubReactor.h 反过来又包含本文件）。
//    具体实现放到 .cpp 里 include，这里只需要类型名字做成员声明即可。
// 4. 后三个 Awaiter 都为 OutboundQueue 出站体系服务：OutboundAwaiter 等队列非空、
//    TransportWriteAwaiter 等 socket 可写、SendCompletionAwaiter 等 ticket 完成。它们
//    共同支撑 writerLoop 协程与主协程的发送完成同步。
// =============================================================================
#pragma once
#ifndef AWAITER_H
#define AWAITER_H
#include <coroutine>
#include <cstdint>
#include "server/transport/ConnectionKey.h"
// 这里只做前向声明，不 include "server/SubReactor/SubReactor.h"。
// 原因：SubReactor.h 又会包含本头文件，互相 include 会造成循环依赖编译失败。
// .cpp 中再 include 真正的头文件使用其成员。
class SubReactor;

/**
 * @brief 读等待器：让协程挂起等待 fd 可读
 *
 * 【通俗解释】
 * 服务员想"听客人说话"（recv 数据），但客人还没张嘴——这时服务员把叫号牌递给
 * epoll（让 epoll 帮忙盯着这位客人），自己挂起去服务别的桌；当客人开口时
 * epoll 通过事件触发把服务员召回，服务员继续 recv 把话听完。
 *
 * await_ready：检查数据是否已经就绪或连接已死，决定要不要直接跳过挂起；
 * await_suspend：登记协程句柄、设置等待状态、调用 updateEvent 武装 epoll；
 * await_resume：协程恢复时清理 waiting 标记。
 */
class ReadAwaiter
{
public:
    // reactor 指向所属 SubReactor（事件循环+连接表），fd 是要等待的 socket
    ReadAwaiter(SubReactor *reactor, ConnectionKey key)
        : key(key), reactor(reactor) {}
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> h);
    void await_resume();
    // auto operator co_await()
    // {
    //     return ReadAwaiter{handle};
    // }
private:
    ConnectionKey key; // fd + 连接代际号，防旧协程命中新连接
    SubReactor *reactor; // 所属反应器（事件循环）
};

/**
 * @brief 写等待器：让协程挂起等待 fd 可写
 *
 * 【通俗解释】
 * 服务员想"对客人说话"（send 数据），但客人耳朵暂时塞着（发送缓冲区满）——
 * 这时服务员把叫号牌递给 epoll，让它盯住"客人耳朵什么时候空出来"（EPOLLOUT），
 * 自己挂起去干别的；等 epoll 通知"可以说了"，服务员回来继续 send。
 * 与 ReadAwaiter 几乎对称，唯一差别是登记的事件类型是 EPOLLOUT 而非 EPOLLIN。
 */
class WriteAwaiter
{
public:
    WriteAwaiter(SubReactor *reactor, ConnectionKey key)
        : key(key), reactor(reactor) {}
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> h);
    void await_resume();

private:
    ConnectionKey key;
    SubReactor *reactor;
};


/**
 * @brief 执行等待器（协程 awaiter）
 *
 * 用于协程挂起等待 Executor 完成 handler 执行。
 * 与 ReadAwaiter/WriteAwaiter 不同，不注册 epoll 事件，
 * 而是等待 Worker 线程完成后通过 completeQueue 唤醒。
 *
 * 【通俗解释】
 * 这是一种"后厨叫号牌"——服务员把一笔订单丢给后厨（Executor 线程池）后，
 * 不会傻等在出餐口，而是挂起去服务别的桌；后厨做完菜后通过 completeQueue
 * 通知前厅，前厅再根据订单号把对应服务员召回继续上菜。整个过程不占用 epoll，
 * 因为后厨不是 I/O 事件，是"业务计算完成"事件。
 */
class ExecuteAwaiter
{
public:
    ExecuteAwaiter(SubReactor *reactor, ConnectionKey key)
        : key(key), reactor(reactor) {}
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> h);
    void await_resume();

private:
    ConnectionKey key;
    SubReactor *reactor;
};

/**
 * @brief 出站队列就绪等待器：让 writerLoop 协程挂起等出站队列非空
 *
 * 【通俗解释】
 * 写工（Writer 协程）想给客人发数据，但出站队列 outboundQueue 现在是空的——
 * 没东西可发。写工把叫号牌递给 SubReactor，自己挂起；当 enqueueOutbound 或
 * processPendingOutbound 把任务塞进队列后，SubReactor 通过 wakeCoroutine(...OUTBOUND)
 * 把写工召回，写工继续 flush 把数据真正 write 到 socket。
 * 与 Read/WriteAwaiter 不同，它等的不是 epoll 事件，而是"队列从空变非空"这一业务状态。
 */
class OutboundAwaiter
{
public:
    OutboundAwaiter(SubReactor *reactor, ConnectionKey key)
        : reactor(reactor), key(key) {}
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> h);
    void await_resume();

private:
    SubReactor *reactor;
    ConnectionKey key;
};

/**
 * @brief 传输层写阻塞等待器：让 writerLoop 协程挂起等 socket 可写
 *
 * 【通俗解释】
 * 写工（Writer 协程）调 transportWriter.flush 想把数据 write 到 socket，但发送
 * 缓冲区满了（EAGAIN）——write 不进去。写工把叫号牌递给 epoll，让它盯住
 * "客人耳朵什么时候空出来"（EPOLLOUT），自己挂起；epoll 通知可写时 SubReactor
 * 通过 wakeCoroutine(...WRITE) 把写工召回，继续 flush。
 * 与 WriteAwaiter 区别：WriteAwaiter 是给主协程（业务发响应）用，TransportWriteAwaiter
 * 是给 Writer 协程（出站冲刷循环）用，但底层都是等 EPOLLOUT。
 */
class TransportWriteAwaiter
{
public:
    TransportWriteAwaiter(SubReactor *reactor, ConnectionKey key)
        : reactor(reactor), key(key) {}
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> h);
    void await_resume();

private:
    SubReactor *reactor;
    ConnectionKey key;
};

/**
 * @brief 发送完成等待器：让协程挂起等 ticket 对应的出站任务被真正发完
 *
 * 【通俗解释】
 * 服务员想确认"我刚才提交的那批出站任务已经全部 write 到 socket 了"再继续往下走
 * （比如 WebSocket 握手响应必须真发出去才能切换协议状态）。服务员先调
 * reserveOutboundTicket 拿一个 ticket，提交任务后 co_await 本 awaiter 把 ticket 交出去
 * 挂起；writerLoop 协程冲刷时把 completedTicket 推进，追上 waitingTicket 后通过
 * wakeCoroutine(...SENT) 把服务员召回。await_ready 直接判断 completedTicket >= ticket，
 * 已发完就立即返回不挂起。
 */
class SendCompletionAwaiter
{
public:
    SendCompletionAwaiter(SubReactor *reactor,
                          ConnectionKey key,
                          uint64_t ticket)
        : reactor(reactor), key(key), ticket(ticket)
    {
    }
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> h);
    void await_resume();

private:
    SubReactor *reactor;
    ConnectionKey key;
    uint64_t ticket;
};

#endif
