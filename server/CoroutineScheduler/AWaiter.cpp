// =============================================================================
// 文件名：AWaiter.cpp
// 职责：实现六种协程等待器（Read/Write/Execute/Outbound/TransportWrite/SendCompletion
//       Awaiter）的挂起-恢复逻辑
//
// 【生活比喻】
// 本文件是"叫号牌"的真实工作流程——服务员（协程）什么时候挂起、怎么挂起、
// 被召回时怎么继续。每张叫号牌都要回答三个问题：
//   1. await_ready：现在是不是已经不用等了？（数据已到 / 连接已死 → 不用等）
//   2. await_suspend：如果要等，怎么登记？把"召回凭证"（协程句柄）留给谁？
//   3. await_resume：被召回时该做哪些收尾？（清掉 waiting 标记）
//
// 关键技术点（初学者重点理解）：
// 1. 所有查找都用 find() 而不是 operator[]：因为 [] 在 key 不存在时会"凭空创建"
//    一个空连接对象（幽灵连接），导致后续逻辑踩坑；find 只查找不创建，安全。
// 2. await_suspend 返回 false 表示不挂起（连接已死的情况），协程会立即继续往下跑，
//    由 Session 在下一行 getConn() 拿到 nullptr 而安全退出。
// 3. 协程挂起期间不能缓存 Connection* 指针——因为别的线程可能在这期间关闭连接，
//    指针会变野。所以每次恢复都要重新 find 查表，这是协程安全的核心铁律。
// 4. OutboundAwaiter/TransportWriteAwaiter 登记 Writer 角色槽位（写工），其他四个
//    登记的是 Main 角色槽位（服务员）。两类协程通过 CoroutineRole 区分，互不串扰。
// =============================================================================
#include "AWaiter.h"
#include "server/SubReactor/SubReactor.h"

// ============================================================================
// ReadAwaiter：让协程挂起等待 fd 可读
// ============================================================================

/**
 * @brief 协程遇到 co_await 时第一个被调用的方法
 * @return true=数据已就绪或连接已死，跳过挂起直接继续；false=需要挂起等待
 *
 * 【通俗解释】
 * 服务员走到客人桌前先看一眼：客人已经把话说完了（数据已到）或者客人已经走了
 * （连接已关闭），那就不用等了直接继续。否则才需要把叫号牌递出去挂起等待。
 */
bool ReadAwaiter::await_ready()
{
     // 连接已关闭时不能再登记 I/O 等待，直接让协程继续，由 Session 的下一次查表决定 co_return。
 // 这里使用 find 而非 operator[]，避免为已关闭 fd 意外创建“幽灵连接”。
    auto *conn = reactor->findConnection(key.fd, key.connId);
    return !conn || conn->state.closed;
}

/**
 * @brief 协程确定要挂起时调用，登记"召回凭证"并武装 epoll
 * @param h 当前协程的句柄（用来将来 resume 唤醒）
 * @return true=真正挂起，把控制权还给调度器；false=连接已死，不挂起直接继续
 *
 * 【通俗解释】
 * 服务员发现客人还没说话，于是：①把工号牌（协程句柄）交给前厅登记；
 * ②告诉前厅"我在等客人开口"（state=READ）；③按下叫号器（updateEvent 武装 EPOLLIN）。
 * 之后服务员就可以放心去服务别的桌了——客人一开口，前厅会通过叫号牌把他召回。
 */
bool ReadAwaiter:: await_suspend(std::coroutine_handle<> h)
{
    // 重新查表：挂起前再确认一次连接是否还在，避免 race condition
    auto *connPtr = reactor->findConnection(key.fd, key.connId);
    if (!connPtr || connPtr->state.closed)
        return false; // 连接已关闭，不挂起（返回 false 让协程立即继续）

    auto &conn = *connPtr;
    // 把协程句柄存进 session 上下文，将来 epoll 事件到来时凭此句柄召回协程
    auto &slot = conn.slot(CoroutineRole::Main);
    slot.handle = h;
    // 标记"我在等读事件"，wakeReadCoroutine 会根据这个状态判断是否该唤醒
    slot.state = AwaitType::READ;
    // waiting=true 表示协程当前处于挂起等待状态
    slot.waiting = true;
   
    // updateEvent 会根据背压决定是否保留 EPOLLIN，并以 ONESHOT 重新武装；
// I/O 到达后 Reactor 只负责把该句柄放回调度器队列。
    reactor->updateEvent(key.fd);
    return true; // 真正挂起协程
}

/**
 * @brief 协程被唤醒恢复执行时调用，做收尾清理
 *
 * 【通俗解释】服务员被召回后，先把"我在等待"的牌子摘下来，然后继续原本的工作。
 */
void ReadAwaiter::await_resume()
{
    // 恢复时再次查表：挂起期间连接可能已被别的线程关闭
    auto *connPtr = reactor->findConnection(key.fd, key.connId);
    if (!connPtr || connPtr->state.closed)
        return; // 连接已关闭，不挂起
    auto &conn = *connPtr;
    // 清掉 waiting 标记，表示协程已经回到运行状态
    conn.slot(CoroutineRole::Main).waiting = false;
}

// ============================================================================
// WriteAwaiter：让协程挂起等待 fd 可写（与 ReadAwaiter 几乎对称）
// ============================================================================

/**
 * @brief 检查是否需要挂起等待可写
 * @return true=连接已死不用等；false=需要挂起等 EPOLLOUT
 */
bool WriteAwaiter::await_ready()
{
    // 修复5：挂起前检查连接是否已关闭
    auto *conn = reactor->findConnection(key.fd, key.connId);
    return !conn || conn->state.closed;
}

/**
 * @brief 登记写等待并武装 EPOLLOUT
 * @param h 协程句柄
 * @return true=挂起；false=连接已死不挂起
 *
 * 【通俗解释】服务员想说话但客人耳朵塞着，于是登记叫号牌让 epoll 盯着
 * "客人耳朵什么时候空出来"。注意 wantWrite=true 会让 updateEvent 把 EPOLLOUT
 * 和原有的 EPOLLIN 合并注册，避免覆盖仍需要的读关注。
 */
bool WriteAwaiter::await_suspend(std::coroutine_handle<> h)
{
    // 修复2+5：用 findConnection，不直接摸 conns
    auto *connPtr = reactor->findConnection(key.fd, key.connId);
    if (!connPtr || connPtr->state.closed)
        return false; // 连接已关闭，不挂起

    auto &conn = *connPtr;
    auto &slot = conn.slot(CoroutineRole::Main);
    slot.handle = h;
    slot.state = AwaitType::WRITE;
    slot.waiting = true;

     // wantWrite 由 updateEvent 与读关注合并，避免注册 EPOLLOUT 时覆盖仍需要的 EPOLLIN。
    // 这是一个关键的"读写在同一 fd 上共存"的设计——既能等读又能等写，互不打架。
    conn.state.wantWrite = true;

    reactor->updateEvent(key.fd);
    return true;
}

/**
 * @brief 写等待恢复时的收尾（本实现为空，因为写完后没有需要清理的状态）
 */
void WriteAwaiter::await_resume()
{
}



// ============================================================================
// ExecuteAwaiter：让协程挂起等待 Executor（后厨线程池）完成 handler 执行
// ============================================================================

// ExecuteAwaiter: 挂起协程等待 Executor 完成 handler 执行。
// 不注册 epoll 事件，由 SubReactor::processComplete 在 Worker 完成后唤醒。

/**
 * @brief 检查是否需要挂起等待后厨
 * @return true=连接已死不用等；false=需要挂起等 Worker 完成
 */
bool ExecuteAwaiter::await_ready()
{
    // 连接已关闭时不挂起，让协程继续走 getConn()==nullptr 的退出路径
    auto *conn = reactor->findConnection(key.fd, key.connId);
    return !conn || conn->state.closed;
}

/**
 * @brief 登记执行等待，但不武装 epoll（因为后厨不是 I/O 事件）
 * @param h 协程句柄
 * @return true=挂起；false=连接已死不挂起
 *
 * 【通俗解释】服务员把订单丢给后厨后挂起等待。这里和读/写等待的关键区别是：
 * 不调 updateEvent 武装 epoll——因为后厨做完菜不是 I/O 事件，epoll 监听不到。
 * 取而代之的是：Worker 线程做完后调用 notifyExecuteComplete 投递完成通知到
 * completeQueue，并通过 eventfd 唤醒 Reactor；Reactor 在 processComplete 里
 * 调用 wakeExecuteCoroutine 把协程召回。这是一条"绕过 epoll 的唤醒通道"。
 */
bool ExecuteAwaiter::await_suspend(std::coroutine_handle<> h)
{
    auto *connPtr = reactor->findConnection(key.fd, key.connId);
    if (!connPtr || connPtr->state.closed)
        return false; // 连接已关闭，不挂起

    auto &conn = *connPtr;
    auto &slot = conn.slot(CoroutineRole::Main);
    slot.handle = h;
    slot.state = AwaitType::EXECUTE;
    slot.waiting = true;
    // 不注册 epoll 事件：唤醒由 processComplete → wakeExecuteCoroutine 完成
    return true;
}

/**
 * @brief 执行等待恢复时清理 waiting 状态
 *
 * 【通俗解释】服务员被后厨召回后，把"我在等后厨"的牌子摘下来，继续端菜上菜。
 */
void ExecuteAwaiter::await_resume()
{
    // 清理 waiting 状态；连接可能已关闭，查表失败是合法路径
    auto *conn = reactor->findConnection(key.fd, key.connId);
    // 判断是否可以直接唤醒
    if (!conn || conn->state.closed)
        return;
    conn->slot(CoroutineRole::Main).waiting = false;
}

// ============================================================================
// OutboundAwaiter：让 writerLoop 协程挂起等出站队列非空（详见 AWaiter.h 类注释）
// ============================================================================

/**
 * @brief 检查是否需要挂起等出站队列非空
 * @return true=连接已死或队列已非空，不用挂起；false=队列为空需要挂起
 */
bool OutboundAwaiter::await_ready()
{
    auto *conn = reactor->findConnection(key.fd, key.connId);
    return !conn || conn->state.closed ||
           !conn->transport.outboundQueue.empty();
}

/**
 * @brief 登记 Writer 协程句柄，挂起等出站队列非空
 * @param h 协程句柄
 * @return true=挂起；false=连接已死或队列已非空，不挂起
 *
 * 【通俗解释】
 * 写工把工号牌递给 SubReactor，标记 state=OUTBOUND；enqueueOutbound/postOutbound
 * 把任务塞进队列后会调 wakeCoroutine(...OUTBOUND) 把写工召回。注意这里登记的是
 * Writer 角色槽位（不是 Main），与主协程的等待互不干扰。
 */
bool OutboundAwaiter::await_suspend(std::coroutine_handle<> h)
{
    auto *conn = reactor->findConnection(key.fd, key.connId);
    if (!conn || conn->state.closed ||
        !conn->transport.outboundQueue.empty())
        return false;
    auto &slot = conn->slot(CoroutineRole::Writer);
    slot.handle = h;
    slot.state = AwaitType::OUTBOUND;
    slot.waiting = true;
    return true;
}

/**
 * @brief 出站等待恢复时的收尾（本实现为空）
 */
void OutboundAwaiter::await_resume()
{
}

// ============================================================================
// TransportWriteAwaiter：让 writerLoop 在 EAGAIN 或公平预算耗尽后挂起并重武装 EPOLLOUT
// ============================================================================

/**
 * @brief 检查是否需要挂起等可写
 * @return true=连接已死不用等；false=需要挂起并把控制权还给 epoll
 */
bool TransportWriteAwaiter::await_ready()
{
    auto *conn = reactor->findConnection(key.fd, key.connId);
    return !conn || conn->state.closed;
}

/**
 * @brief 登记 Writer 协程句柄，武装 EPOLLOUT
 * @param h 协程句柄
 * @return true=挂起；false=连接已死不挂起
 *
 * 【通俗解释】
 * 与 WriteAwaiter 几乎对称，差别仅在于登记的是 Writer 角色槽位（写工）而非 Main
 * （服务员）。设 wantWrite=true 让 updateEvent 把 EPOLLOUT 与 EPOLLIN 合并武装，
 * 等 epoll 通知可写时 wakeCoroutine(...WRITE) 把写工召回继续 flush。
 */
bool TransportWriteAwaiter::await_suspend(std::coroutine_handle<> h)
{
    auto *connPtr = reactor->findConnection(key.fd, key.connId);
    if (!connPtr || connPtr->state.closed)
        return false;
    auto &conn = *connPtr;
    auto &slot = conn.slot(CoroutineRole::Writer);
    slot.handle = h;
    slot.state = AwaitType::WRITE;
    slot.waiting = true;
    conn.state.wantWrite = true;
    reactor->updateEvent(key.fd);
    return true;
}

/**
 * @brief 写阻塞或公平让出后恢复时的收尾（本实现为空）
 */
void TransportWriteAwaiter::await_resume()
{
}

// ============================================================================
// SendCompletionAwaiter：让协程挂起等 ticket 对应的出站任务被真正发完
// ============================================================================

/**
 * @brief 检查是否需要挂起等发送完成
 * @return true=连接已死或 ticket 已被 completedTicket 追上，不用挂起；false=需要挂起
 */
bool SendCompletionAwaiter::await_ready()
{
    auto *conn = reactor->findConnection(key.fd, key.connId);
    return !conn || conn->state.closed ||
           conn->transport.completedTicket >= ticket;
}

/**
 * @brief 登记 Main 协程句柄，记录 waitingTicket，挂起等 SENT 唤醒
 * @param h 协程句柄
 * @return true=挂起；false=连接已死或已完成，不挂起
 *
 * 【通俗解释】
 * 服务员把 ticket 写进 conn.transport.waitingTicket，再登记 Main 协程槽位
 * state=SENT；writerLoop 冲刷时把 completedTicket 推进，追上 waitingTicket 后
 * 调 wakeCoroutine(...SENT) 把服务员召回。
 */
bool SendCompletionAwaiter::await_suspend(std::coroutine_handle<> h)
{
    auto *connPtr = reactor->findConnection(key.fd, key.connId);
    if (!connPtr || connPtr->state.closed ||
        connPtr->transport.completedTicket >= ticket)
        return false;
    auto &conn = *connPtr;
    conn.transport.waitingTicket = ticket;
    auto &slot = conn.slot(CoroutineRole::Main);
    slot.handle = h;
    slot.state = AwaitType::SENT;
    slot.waiting = true;
    return true;
}

/**
 * @brief 发送完成恢复时清掉 waitingTicket
 *
 * 【通俗解释】服务员被召回后清掉 waitingTicket=0，表示"不再等任何 ticket"。
 * 连接可能已关闭，查表失败直接返回。
 */
void SendCompletionAwaiter::await_resume()
{
    if (auto *conn = reactor->findConnection(key.fd, key.connId))
        conn->transport.waitingTicket = 0;
}
