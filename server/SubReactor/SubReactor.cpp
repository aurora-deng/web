// =============================================================================
// 文件名：SubReactor.cpp
// ------------------------------------------------------------
// 【职责比喻：车间主任 / 护士长 / 楼层经理的工作手册】
// 实现"从 Reactor" SubReactor 的核心逻辑——事件循环（loop）、连接管理
// （addFd/processPendingFds/fd_close）、I/O 读写（recvSocket）、协程唤醒
// （wakeRead/Write/ExecuteCoroutine）、跨线程通信（notifyExecuteComplete/processComplete）。
// 本文件是"楼层经理"的完整工作手册：
//   - loop()：经理全天候坐在监控台前，等铃响（epoll_wait），按铃类型分流处理；
//   - addFd：前台把客人房号送来，经理先记下来，等下批处理（processPendingFds）；
//   - recvSocket：客人发话时，经理把话一字不漏记下来（recv 直到 EAGAIN）；
//   - wakeReadCoroutine：客人发话后，经理把对应的客房服务员叫醒去听；
//   - fd_close：客人要退房或被超时清退时，经理统一办退房手续，注意服务员正在后厨
//     做菜的话要留 zombieWakes 条子，免得菜做好回来找不到服务员导致泄漏。
//
// 【第四阶段重构在 cpp 中的体现】
// processPendingFds() 创建 HttpSession 时多传一个 router 参数：
//   make_shared<HttpSession>(fd, this, router)
// 第四阶段前是 make_shared<HttpSession>(fd, this) —— 那时 HttpSession 用 SubReactor 持有的
// 共享 codec。重构后 codec 下沉到 Session 内部，HttpSession 构造时需要 router 来：
//   ①内部自建 HttpCodec（不再共享 SubReactor 的 codec）；
//   ②按 URL 分发到对应 handler。SubReactor 自己则完全不碰协议编解码。
//
// 关键技术点（初学者重点理解）：
// 1. 单线程所有权规则：conns/wheel/epoll_ctl 只在 SubReactor 自己的线程操作；
//    跨线程入口（addFd / notifyExecuteComplete）只做"投递队列 + 写 eventfd"两步，
//    真正处理由本线程在 processPendingFds / processComplete 完成——这是无锁化的关键。
// 2. eventfd + notified 合并：高并发接入时连续 addFd 不必每次都 write eventfd，
//    用 atomic<bool> notified 合并唤醒，一次 write 即可让本线程处理积压的全部 fd。
// 3. processComplete 的 connId 匹配：fd 会被内核复用，光看 fd 不能确定是同一连接；
//    用 connId（全局自增）匹配，能识别"fd 复用了但已是不同连接"的旧通知并安全丢弃。
// 4. zombieWakes 防协程泄漏：fd_close 时若协程正在 Executor 跑（state=EXECUTE），
//    连接被删后协程句柄存进 zombieWakes，Worker 完成后 processComplete 凭 connId 找回
//    并 schedule 它，让协程恢复后通过 getConn()==nullptr 安全退出。
// 5. updateEvent 的背压合并：把"是否暂停读"（pauseByMemory）和"是否要写"（wantWrite）
//    综合成一个 epoll 事件掩码 rearm，避免分别操作 epoll 引发的事件覆盖。
// 6. 【OutboundQueue 出站路径】出站不通过 Session 抽象 sender，统一走 OutboundTask +
//    OutboundQueue + TransportWriter + writerLoop 体系：本线程入队 enqueueOutbound、
//    跨线程投递 postOutbound（先入 OutboundQueue::pending，再写 eventfd 由本线程在
//    processPendingOutbound 消费）、预约序号 reserveOutboundTicket、协程冲刷 writerLoop
//    （空队列时 OutboundAwaiter 挂起、写阻塞时 TransportWriteAwaiter 挂起）。
// 7. 【SessionFactory 依赖注入】协议升级（HTTP→WebSocket）所需的子类创建由
//    sessionFactory_ 完成，本类只持有 SessionFactory* 抽象指针。SessionFactory 由
//    ServerRuntime 创建，经 ReactorGroup::start → SubReactor::setSessionFactory 注入。
// =============================================================================

#include "SubReactor.h"
#include "log/logger/logger.h"
#include "server/CoroutineScheduler/AWaiter.h"
#include "server/Executor/Executor.h"
#include "server/Route/Router.h"
#include "server/http/HttpSession/HttpSession.h"
#include "server/session/Session/Session.h"
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
std::atomic<uint64_t> global_conn_id{0}; // 自增

SubReactor::SubReactor(Router &router, Executor &executor)
    : wheel(slotNum, timeout),
      transportWriter(segPool, wheel),
      outbound_(
          transportWriter,
          [this](int fd)
          {
              wakeCoroutine(fd, CoroutineRole::Writer, AwaitType::OUTBOUND);
          },
          [this](int fd) { updateEvent(fd); }),
      router_(router),
      executor_(executor)
{
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1)
        throw std::runtime_error(
            std::string("subreactor epoll_create1: ") + std::strerror(errno));

    event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd == -1)
    {
        close(epfd);
        throw std::runtime_error(
            std::string("subreactor eventfd: ") + std::strerror(errno));
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = event_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &event) == -1)
    {
        close(event_fd);
        close(epfd);
        throw std::runtime_error(
            std::string("subreactor epoll_ctl: ") + std::strerror(errno));
    }

    wheel.setCloseCallbace([this](int fd) { onWheelTimeout(fd); });
    scheduler_.setCompletionCallback(
        [this](int fd,
               uint64_t connId,
               CoroutineRole role,
               std::coroutine_handle<> handle)
        {
            auto it = conns.find(fd);
            if (it == conns.end() || it->second->id != connId)
                return;
            auto &slot = it->second->slot(role);
            if (slot.handle &&
                slot.handle.address() == handle.address())
            {
                slot.handle = nullptr;
                slot.state = AwaitType::NONE;
                slot.waiting = false;
            }
        });
}

/**
 * @brief 析构函数：先停止并 join 线程，再关闭所有 fd
 *
 * 【通俗解释】
 * 楼层经理离职流程：①stop() 通知自己的线程打烊；②join() 等线程真正退出；
 * ③线程退出后无人再访问 conns/epfd，可以安全地挨个 close 所有客人 fd，再关 eventfd/epfd。
 * 顺序很重要——必须先 join 再 close，否则线程还在跑就关 epfd 会段错误。
 */
SubReactor::~SubReactor()
{
    // 先停止后散出
    stop();
    join();

    // 线程退出后不再有 epoll/连接表并发访问；直接关闭传输资源。
    // 协程帧随后由 CoroutineScheduler 析构统一销毁。
    for (auto &[fd, _] : conns)
        close(fd);
    conns.clear();
    if (event_fd >= 0)
        close(event_fd);
    if (epfd >= 0)
        close(epfd);
}

// 线程关闭策略
/**
 * @brief 阻塞等待本 Reactor 的事件循环线程退出
 *
 * 【通俗解释】等本楼层的服务员线程干完手上的活并真正下班。
 */
void SubReactor::join()
{
    if (th.joinable())
        th.join();
}

// 使用关闭触发唤醒策略，如果关闭就发送fd使得epoll知道，逻辑参考单个reactor里面的函数
/**
 * @brief 通知事件循环线程停止
 *
 * 【通俗解释】
 * 经理给监控台发"打烊"信号：①running.exchange(false) 标记打烊（原子操作）；
 * ②写 eventfd 唤醒可能正在 epoll_wait 阻塞的线程，让它醒来检查 running 并退出。
 * 如果早已停止（exchange 返回 false），直接返回避免重复操作。
 */
void SubReactor::stop()
{
    // 判断是否早已关闭
    if (!running.exchange(false))
        return;
    uint64_t one = 1;
    // 发送关闭通知
    if (event_fd >= 0)
        (void)write(event_fd, &one, sizeof(one));
}

/**
 * @brief 综合背压和写关注，重新武装 fd 的 epoll 事件
 * @param fd 目标 socket
 *
 * 【通俗解释】
 * 经理重新调节监控台对某客房的注意力：
 *   - 如果读缓冲区已爆（pauseByMemory），就不监听 EPOLLIN（暂停读，等积压消化）；
 *   - 如果服务员说"要写"（wantWrite），就加上 EPOLLOUT；
 *   - 两者合并后调 rearm 用 EPOLLONESHOT 重新武装。
 * 关键设计：合并事件避免分别 MOD 导致后一次覆盖前一次。例如只想加 EPOLLOUT
 * 却忘了带上 EPOLLIN，会把仍需要的读关注丢掉。
 */
void SubReactor::updateEvent(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    uint32_t ev = 0;
    // 背压修复处：综合所有背压条件决定是否暂停读
    // 任一背压条件触发都应暂停读，防止数据继续涌入

    it->second->state.readPaused =
        it->second->state.pauseByMemory ||
        it->second->transport.pauseByWrite;
    if (!it->second->state.readPaused)
        ev |= EPOLLIN;

    if (it->second->state.wantWrite)
    {
        ev |= EPOLLOUT;
    }

    rearm(fd, ev);
}

/**
 * @brief 唤醒等待读的协程
 * @param fd 数据到达的 socket
 *
 * 【通俗解释】
 * 客人发话了（EPOLLIN），经理去找该客房的专属服务员，让他回来继续 recv。
 * 三道防御：①连接不存在直接返回；②协程不在 READ 状态直接返回（可能已在跑或已唤醒）；
 * ③handle 无效或已 done 直接返回。所有唤醒统一走 scheduler.schedule 去重，
 * 避免重复入队导致重复 resume。
 */
void SubReactor::wakeReadCoroutine(int fd)
{
    wakeCoroutine(fd, CoroutineRole::Main, AwaitType::READ);
}

// 安全唤醒写协程，统一由 scheduler.schedule 去重。
/**
 * @brief 唤醒等待写的协程（与 wakeReadCoroutine 对称）
 * @param fd 可写的 socket
 */
void SubReactor::wakeWriteCoroutine(int fd)
{
    wakeCoroutine(fd, CoroutineRole::Main, AwaitType::WRITE);
    wakeCoroutine(fd, CoroutineRole::Writer, AwaitType::WRITE);
}

void SubReactor::wakeCoroutine(int fd, CoroutineRole role, AwaitType expected)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    auto &ctx = it->second->slot(role);
    if (ctx.state != expected)
        return;
    if (!ctx.handle || ctx.handle.done())
        return;
    ctx.state = AwaitType::NONE;
    ctx.waiting = false;
    auto h = ctx.handle;
    if (h)
    {
        scheduler_.schedule(h);
    }
}

// 由于在io端的线程和在业务端的线程分离，所以协程唤醒需要有单独的函数来实现唤醒
// 唤醒执行完成的协程：Worker 线程完成 handler 后通过 processComplete 调用，唤醒协程继续进行下一步操作。
/**
 * @brief 唤醒等待 Executor 完成的协程
 * @param fd 业务完成的 socket
 *
 * 【通俗解释】
 * 后厨做完菜了（handler 跑完），经理把对应客房的服务员叫回来端菜上菜。
 * 只能由本 Reactor 线程调用（processComplete 内），所以无需加锁。
 */
void SubReactor::wakeExecuteCoroutine(int fd)
{
    wakeCoroutine(fd, CoroutineRole::Main, AwaitType::EXECUTE);
}

// Worker 线程调用（线程安全）：投递完成通知到 completeQueue，并通过 eventfd 唤醒 Reactor。
// 跨线程通信通知函数，通知work线程工作，实现队列缓存，保障跨线程通信安全，同时使用write提醒竹reactor
/**
 * @brief Worker 线程完成 handler 后调用，投递完成通知（线程安全）
 * @param fd 业务完成的 socket
 * @param connId 完成时的连接 id（用于识别"fd 复用但已是不同连接"的旧通知）
 *
 * 【通俗解释】
 * 后厨做完菜，按铃通知前厅：①加锁把 (fd, connId) 推进 completeQueue；
 * ②用 atomic exchange 合并 eventfd 写入，避免高并发下每个任务都 write 一次系统调用；
 * ③write eventfd 唤醒本 Reactor 线程，它会在 processComplete 里处理这个完成通知。
 * 这是 Worker 线程 → Reactor 线程的标准跨线程通信通道。
 */
void SubReactor::notifyExecuteComplete(int fd, uint64_t connId)
{
    {
        std::lock_guard<std::mutex> lock(completeMtx);
        completeQueue.push({fd, connId});
    }
    // notified exchange 合并：连续多次完成只 write eventfd 一次，
    // 本线程醒来后会一次性 swap 整个队列处理，省系统调用
    if (!notified.exchange(true))
    {
        uint64_t one = 1;
        if (write(event_fd, &one, sizeof(one)) == -1)
        {
            if (errno != EAGAIN)
            {
                LOG_INFO(std::string("notifyExecuteComplete eventfd write error: ") + strerror(errno));
            }
        }
    }
}

// Reactor 线程消费（业务处理完成）完成队列：匹配 connId 后唤醒对应协程。
// 批量交换队列减少锁竞争，处理僵尸唤醒防止协程泄漏。
/**
 * @brief Reactor 线程消费完成队列：匹配 connId 后唤醒协程
 *
 * 【通俗解释】
 * 经理从出餐窗口一次性把所有做好的菜端走（swap 整个 completeQueue 到 local），
 * 然后挨个核对订单号（connId）：①订单还在 → wakeExecuteCoroutine 叫服务员端菜；
 * ②订单已退房（fd_close 时协程正在后厨做菜）→ 查 zombieWakes 找到条子，
 * 直接 schedule 协程让它恢复后通过 getConn()==nullptr 安全退出。
 * 关键点：用 connId 而非 fd 匹配，因为 fd 可能已被复用给新连接。
 */
void SubReactor::processComplete()
{
    std::queue<std::pair<int, uint64_t>> local;
    {
        // 一次性 swap 整个队列，减少锁持有时间，让 Worker 线程能继续 push
        std::lock_guard<std::mutex> lock(completeMtx);
        local.swap(completeQueue);
    }
    // 将队列交换回来，开始继续执行
    while (!local.empty())
    {
        auto [fd, connId] = local.front();
        local.pop();

        auto it = conns.find(fd);
        if (it != conns.end() && it->second->id == connId)
        {
            // 连接仍存活且 id 匹配：唤醒协程继续端菜
            wakeExecuteCoroutine(fd);
            continue;
        }
        // 连接已关闭：检查僵尸唤醒表，防止协程泄漏
        // 场景：fd_close 时协程正在 Executor 中跑，handle 存进了 zombieWakes
        auto zit = zombieWakes.find(connId);
        if (zit != zombieWakes.end())
        {
            auto h = zit->second;
            zombieWakes.erase(zit);
            if (h && !h.done())
                scheduler_.schedule(h); // 让协程恢复后通过 getConn()==nullptr 安全退出
        }
    }
}


/**
 * @brief 角色名转字符串（仅供日志使用）
 * @param role 协程角色（Main/Writer/Timer）
 * @return 角色名 C 字符串
 *
 * 【通俗解释】
 * 把 CoroutineRole 枚举翻译成可读字符串，仅供 LOG_DEBUG 打印 fd_close 原因时使用，
 * 让日志能看出是哪个角色（Main 主协程 / Writer 写协程 / Timer 定时器）触发了关闭。
 */
const char *SubReactor::roleName(CoroutineRole role)
{
    switch (role)
    {
    case CoroutineRole::Main:
        return "Main";
    case CoroutineRole::Writer:
        return "Writer";
    default:
        return "Timer";
    }
}

// 统一套接字关闭，并维持"只有所属 Reactor 关闭连接"的所有权规则。
/**
 * @brief 统一套接字关闭流程
 * @param fd 要关闭的 socket
 * @param reason 关闭原因（用于日志）
 * @param initiator 触发关闭的协程角色；fd_close 内部会跳过唤醒该角色，避免自唤醒
 *
 * 【通俗解释】
 * 经理办退房手续的完整流程：
 *   ①查表不存在/已 closed → 直接返回（防重复关闭）；
 *   ②遍历每个角色的协程句柄，判断该协程当前是否正在 Executor 跑（executing）；
 *   ③若该角色 == initiator → 跳过（不唤醒自己），continue；
 *   ④其他角色且 executing → 把 handle 存 zombieWakes，等 Worker 完成后
 *     processComplete 凭 connId 找回它，避免泄漏；
 *   ⑤其他角色且 !executing → 直接 schedule，让它恢复后通过 getConn()==nullptr
 *     安全 co_return；
 *   ⑥从时间轮移除、设 closed 标记、epoll_ctl DEL、close fd、erase 出 conns、计数减一。
 *   所有权铁律：只有本 Reactor 线程能调 fd_close，避免与 conns 操作竞争。
 */
void SubReactor::fd_close(int fd,
                          std::string_view reason,
                          CoroutineRole initiator)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;

    // 优化：加上状态检查，防止当某fd已经关闭之后重复关闭或者关闭之后任然在fd
    if (it->second->state.closed)
        return;

    const uint64_t connId = it->second->id;
    LOG_DEBUG(std::string("[CLOSE] fd=") + std::to_string(fd) +
              " connId=" + std::to_string(connId) +
              " reason=" + std::string(reason) +
              " initiator=" + roleName(initiator));

    if (it->second->session)
        it->second->session->onClose();

    // 连接销毁前给所有尚未写完的任务留下终态，避免回执永久停在 Pending。
    transportWriter.cancelAll(*it->second, OutboundOutcome::Closed);

    std::array<std::coroutine_handle<>,
               static_cast<std::size_t>(CoroutineRole::Count)>
        coroutinesToWake{};
    for (std::size_t i = 0; i < it->second->coroutineSlots.size(); ++i)
    {
        const auto role = static_cast<CoroutineRole>(i);
        auto &slot = it->second->coroutineSlots[i];
        const auto h = slot.handle;
        const bool executing = slot.state == AwaitType::EXECUTE;

        slot.handle = nullptr;
        slot.state = AwaitType::NONE;
        slot.waiting = false;

        if (!h || role == initiator)
            continue;
        if (executing)
            zombieWakes[connId] = h;
        else
            coroutinesToWake[i] = h;
    }

    // 删除对应时间轮
    wheel.remove(fd);

    it->second->state.closed = true;

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    conns.erase(it);
    activeConns_.fetch_sub(1, std::memory_order_relaxed);

    for (const auto h : coroutinesToWake)
        if (h)
            scheduler_.schedule(h);
}
// 设置fd为非堵塞，对于新添加的fd都要使用
/**
 * @brief 把 fd 设为非阻塞模式
 * @param fd 目标 socket
 *
 * 【通俗解释】
 * 让 recv/send 不会卡住线程：没有数据时立即返回 EAGAIN 而不是阻塞。
 * 这是 epoll + 协程模型的基础——所有 I/O 都必须非阻塞，否则会卡死事件循环。
 */
void SubReactor::fd_unblock(int fd)
{

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
// 重新唤醒、
/**
 * @brief 用 EPOLLONESHOT 重新武装 fd
 * @param fd 目标 socket
 * @param events 要监听的事件掩码（EPOLLIN/EPOLLOUT 等）
 *
 * 【通俗解释】
 * EPOLLONESHOT 模式下，fd 触发一次事件后会自动"哑火"，必须再次 rearm 才能继续监听。
 * 这保证了同一 fd 的事件不会并发触发——一次只唤醒一次协程，处理完再重新武装。
 * 加上 EPOLLET（边缘触发）+ EPOLLRDHUP（对端关闭），覆盖正常读、写、对端断开三种情况。
 */
void SubReactor::rearm(int fd, uint32_t events)
{
    // 加上rearm检查，防止已关闭的fd重新rearm
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    if (it->second->state.closed)
        return;
    epoll_event ev{};
    ev.events = EPOLLONESHOT | EPOLLET | EPOLLRDHUP | events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1)
    {
        LOG_INFO(std::string("epol_ctl MOD failed") + strerror(errno));
    }
}

/**
 * @brief 启动事件循环线程
 *
 * 【通俗解释】经理上岗：开一条新线程跑 loop()，自己在后台监控台值班。
 */
void SubReactor::run()
{
    // 获得属于自己的线程
    th = std::thread([this]
                     {
            // 放置要处理的业务逻辑函数
            loop(); });
}

/**
 * @brief 从 socket 读取数据到 readBuffer
 * @param fd 要读的 socket
 * @return RecvState 读取结果状态
 *
 * 【通俗解释】
 * 客人发话了，经理把话一字不漏记到本子上：
 *   ①预查 conns 防止虚空索敌（连接已删则返回 CLOSED）；
 *   ②检查读缓冲水位，超过 MAX_PENDING_BYTES 触发背压（pauseByMemory=true），
 *     updateEvent 暂停 EPOLLIN，返回 PAUSED；
 *   ③循环 recv 直到 EAGAIN（非阻塞模式下表示"暂无更多数据"）；
 *   ④每次成功 recv 后刷新时间轮（wheel.refresh），表示"客人还活着"；
 *   ⑤res==0 表示对端 FIN（半关闭），不立即 fd_close 而是设 peerClosed=true，
 *     让 Session 把已读入的最后一个请求处理完再关；
 *   ⑥再次检查背压，超过水位返回 PAUSED。
 *
 * 【性能修复】
 * recv 缓冲区从 8KB 提升到 64KB，单次 recv 即可读完一个典型请求，
 * 减少系统调用次数。64KB 接近 TCP 默认窗口规模，是经验最优值。
 */
RecvState SubReactor::recvSocket(int fd)
{
    // 说明有东西从客户端反过来，准备接受东西，同时将整个任务进行处理

    // 预查询,防止虚空索敌链接幽灵对象
    auto it = conns.find(fd);
    if (it == conns.end())
        return RecvState::CLOSED;

    auto &conn = *it->second;

    // 背压修复处：使用 readableBytes() 代替 buf.size() 判断内存水位
    // readableBytes() 是未读数据大小，buf.size() 是 vector 总容量（含已读和空闲）
    // 原代码用 buf.size() 导致已解析数据仍计入水位，背压判断不准确
    if (conn.readBuffer.readableBytes() > kMaxPendingReadBytes)
    {
        conn.state.pauseByMemory = true;
        updateEvent(fd);
        return RecvState::PAUSED;
    }
    // 开始接受数据
     while (running.load(std::memory_order_acquire))
    {
        // 性能修复处：recv 缓冲区从 8KB 提升到 64KB
        // 原代码每次最多读 8KB， 请求需要多次 recv 系统调用
        // 64KB 接近 TCP 默认窗口规模，单次 recv 即可读完一个典型请求
        char buffer[65536];
        // 接收消息
        int res = recv(fd, buffer, sizeof(buffer), 0);

        // 进行分类处理判断
        if (res == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // printf("数据已经读完\n");
                break; // 非阻塞模式下 EAGAIN 表示"暂无更多数据"，本轮读完
            }
            else if (errno == EINTR)
            {
                continue; // 被信号中断，重试
            }
            else
            { // 连接失败
                // printf("连接失败\n");
                fd_close(fd, "连接失败", CoroutineRole::Main);
                return RecvState::CLOSED;
            }
        }
        else if (res == 0)
        {
            // TCP FIN 可能和最后一个完整请求同时到达。立即 fd_close 会丢弃已经读入的请求；
            // 记录半关闭状态，让 Session 在能够完整解析时发完最后一个响应。
            conn.state.peerClosed = true;
            break;
        }
        // printf("收到数据：%.*s\n", res, buffer);

        // 开始处理数据
        conn.readBuffer.append(buffer, res);
        conn.pendingBytes = conn.readBuffer.readableBytes();
        if (conn.pendingBytes > kMaxPendingReadBytes)
        {
            conn.state.pauseByMemory = true;
            updateEvent(fd);
            return RecvState::PAUSED;
        }

        // 遇到有用请求，刷新请求.防止一直接受不到conn
        wheel.refresh(fd);
        // 可优化点：使用零拷贝  std::string localBuf.swap(conns[fd].readBuffer);
        //  或者使用现在的多reactor直接对conns进行操作
    }
    return RecvState::READY;
}


/**
 * @brief 事件循环主体（在本 Reactor 自己的线程内运行）
 *
 * 【通俗解释】
 * 经理全天候值班的核心流程：
 *   while (running) {
 *     ①epoll_wait 等事件（最多 100ms 超时，平衡响应性和 CPU 占用）；
 *     ②对每个事件分流：
 *        - event_fd 触发 → 排空 eventfd、重置 notified、processPendingFds + processComplete；
 *        - 普通 fd 触发 → 按事件类型（EPOLLERR/EPOLLHUP/EPOLLOUT/EPOLLIN）唤醒对应协程；
 *     ③每秒 tick 一次时间轮（清理超时连接）；
 *     ④scheduler.runReady() 把所有就绪协程挨个 resume 直到队列空。
 *   }
 * 关键设计：所有 conns/epoll 操作都在本线程做，无锁；eventfd 是唯一的跨线程入口。
 */
void SubReactor::loop()
{
    // 创建epoll储存大小
    epoll_event events[kMaxEvents];
    auto last = std::chrono::steady_clock::now();
    // stop() 会清 running 并写 eventfd；必须在每次 wait 后检查，否则 join() 永久挂起。
    while (running.load(std::memory_order_acquire))
    {
        // 开始监听epfd并将数据存放到events中,优化100ms无连接超时
        // 性能修复处：epoll_wait 超时从 1000ms 降为 100ms
        // 原代码 1000ms 导致新事件最多等 1 秒才被处理，高并发下延迟飙升
        // 100ms 在响应性和 CPU 占用之间取得平衡，同时保证时间轮每秒 tick 精度
        int n = epoll_wait(epfd, events, kMaxEvents, 100);
        if (!running.load(std::memory_order_acquire))
            break;
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            LOG_ERROR(std::string("subreactor epoll_wait: ") + strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            // 就处理读出和写入
            if (fd == event_fd)
            {
                // eventfd 可读：排空计数器，避免多次唤醒堆积
                uint64_t cnt;
                while (read(event_fd, &cnt, sizeof(cnt)) > 0)
                    ;

                // eventfd 优化处：读完 eventfd 后重置 notified 标志
                // 这样后续的 pushResult/notifyStream/addFd 才能再次触发 eventfd 写入
                // 必须在 read 之后、处理任务之前重置，确保不丢失新入队的任务
                notified.store(false, std::memory_order_release);

                // 段错误修复处：先处理待添加的 fd，再处理任务结果
                // 确保所有对 conns/wheel/epoll_ctl 的操作都在 SubReactor 线程中完成
                // 派发任务
                processPendingFds();
                // 处理已完成任务并唤醒协程（请求解析）
                processComplete();
                processPendingOutbound();
            }
            else
            {
                if (events[i].events & EPOLLERR)
                {
                    fd_close(fd, "epoll error");
                    continue;
                }
                // 处理半关闭情况，主要就是防止出现信息还没有发送完直接关闭连接了
                if (events[i].events & (EPOLLHUP | EPOLLRDHUP))
                {
                    auto it = conns.find(fd);
                    if (it != conns.end())
                        it->second->state.peerClosed = true;
                }
                if (events[i].events & EPOLLOUT) //-------处理write-send发出
                {
                    wakeWriteCoroutine(fd);
                }

                if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) // 处理监听接受/半关闭
                {
                    wakeReadCoroutine(fd);
                }
                // scheduler.runReady();
            }
        }

        // 检查超时
        auto now = std::chrono::steady_clock::now();

        auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - last).count(); // 刷新计时
        if (sec >= 1)
        {
            wheel.tick(); // 每秒 tick 一次时间轮，清理超时连接
            last = now;
        }

        scheduler_.runReady(); // 把所有就绪协程挨个 resume 直到队列空
        // currentTick++;
    }
}

// 段错误修复处：addFd 改为只将 fd 放入待处理队列，由 SubReactor 线程完成实际注册
// 原代码在主线程直接操作 conns/wheel/epoll_ctl，与 SubReactor 线程竞争
// 导致 unordered_map rehash 时迭代器失效 → free(): invalid pointer
/**
 * @brief 跨线程入口：把新 fd 投递到本 Reactor（线程安全）
 * @param fd 新接受的 socket
 *
 * 【通俗解释】
 * 前台把客人房号送过来：①设 fd 非阻塞；②加锁 push 进 pendingFds 队列；
 * ③用 atomic exchange 合并 eventfd 写入，只唤醒一次本 Reactor 线程。
 * 注意：这里只投递队列，不直接操作 epoll/conns——那些必须由本 Reactor 自己的线程
 * 在 processPendingFds 里完成，否则会与事件循环竞争导致段错误。
 */
void SubReactor::addFd(int fd)
{
    // 判断当前线程是否已经运行
    if (!running.load(std::memory_order_acquire))
    {
        close(fd); // Reactor 已停，直接关 fd 防泄漏
        return;
    }
    // 设置为非堵塞（fcntl 是系统调用，线程安全，可以在主线程做）
    fd_unblock(fd);

    // 将 fd 放入待处理队列
    {
        std::lock_guard<std::mutex> lock(pending_mtx);
        pendingFds.push(fd);
    }

    // eventfd 优化处：使用 atomic<bool> + exchange 合并唤醒
    // 连续 addFd 只 write eventfd 一次，本线程醒来后一次性处理全部待添加 fd
    if (!notified.exchange(true))
    {
        uint64_t one = 1;
        if (write(event_fd, &one, sizeof(one)) == -1)
        {
            if (errno != EAGAIN)
            {
                LOG_INFO(std::string("addFd eventfd write error: ") + strerror(errno));
            }
        }
    }
}

// 段错误修复处：由 SubReactor 线程调用，处理待添加的 fd 队列
// 所有对 conns/wheel/epoll_ctl 的操作都在 SubReactor 线程中完成，消除数据竞争
/**
 * @brief 处理待添加的 fd 队列（仅本 Reactor 线程调用）
 *
 * 【通俗解释】
 * 经理批量办入住：①swap 出整个 pendingFds 到 local（减少锁持有时间）；
 * ②挨个 fd 办入住手续：epoll_ctl ADD 注册 → 创建 Connection 对象 →
 *   创建 HttpSession → 启动协程 task → adopt 进调度器 → runReady 让协程跑到第一个 co_await。
 * 关键点：所有 epoll/conns/wheel 操作都在本线程做，彻底消除数据竞争。
 */
void SubReactor::processPendingFds()
{
    // 直接交换获得所有数据到本地来慢慢处理
    std::queue<int> local;
    {
        std::lock_guard<std::mutex> lock(pending_mtx);
        local.swap(pendingFds);
    }
    while (!local.empty())
    {
        int fd = local.front();
        local.pop();

        if (!running.load(std::memory_order_acquire))
        {
            close(fd); // 打烊中，直接关 fd
            continue;
        }

        epoll_event ev{};
        // EPOLLONESHOT: 触发一次后哑火，需 rearm；EPOLLET: 边缘触发；
        // EPOLLRDHUP: 监听对端关闭。初始只监听读，写按需添加。
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

        // 初始化conns对象
        auto conn = std::make_unique<Connection>();

        conn->fd = fd;
        conn->id = ++global_conn_id; // 全局唯一递增 id，用于 processComplete 识别 fd 复用
        conn->state.readPaused = false;

        auto *raw = conn.get();
        conns.emplace(fd, std::move(conn));
        activeConns_.fetch_add(1, std::memory_order_relaxed);

        // 创建专属服务员（HttpSession），传入本 Reactor 指针和路由表。
        //
        // 【第四阶段关键：为什么多传一个 router】
        // 第四阶段前是 make_shared<HttpSession>(fd, this) —— 那时 HttpSession 用 SubReactor
        // 持有的共享 HttpCodec。重构后 codec 下沉到 Session 内部，HttpSession 构造时需要
        // router 来：①内部自建 HttpCodec（每条连接独立，互不干扰）；②按 URL 分发到 handler。
        // 赋值给 raw->session（shared_ptr<Session> 基类指针）——这样后续若升级为 WebSocket，
        // 可平滑替换为 WebSocketSession 实例，无需改 Connection 结构。
        raw->session = std::make_shared<HttpSession>(raw->key(), this, router_);
        // conns[fd] = conn;
        // 启动会话协程：调 run() 返回 Task（lazy，未实际执行）
        auto task = raw->session->run();
        // release 把协程句柄所有权转交给调度器
        auto h = task.release();
        raw->slot(CoroutineRole::Main).handle = h;

        // 设置对应时间轮：超时未活动会被 wheel 回调 fd_close
        wheel.add(fd);

        // adopt 把协程登记进调度器并立即入队；owner=session 共享指针保证协程存活期间 session 不析构
        scheduler_.adopt(
            fd, raw->id, CoroutineRole::Main, h, raw->session);
        auto writerTask = writerLoop(fd, raw->id);
        auto writerHandle = writerTask.release();
        raw->slot(CoroutineRole::Writer).handle = writerHandle;
        scheduler_.adopt(
            fd,
            raw->id,
            CoroutineRole::Writer,
            writerHandle);
        scheduler_.runReady(); // 启动到第一个co_await
    }
}

uint64_t SubReactor::nowSec()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/**
 * @brief 查表入口（仅按 fd）
 * @param fd 目标 socket
 * @return 命中的 Connection*；不在表中返回 nullptr
 *
 * 【通俗解释】
 * 外部（Session/Awaiter）查连接的唯一合法入口——conns 表本身是 private，外部不得直接摸。
 * 注意：返回裸指针，调用方拿到后若跨"投递到本线程"边界还需用 connId 重验，避免 fd 复用陷阱。
 */
Connection *SubReactor::findConnection(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return nullptr;
    return it->second.get();
}

/**
 * @brief 查表入口（fd + connId 双重校验）
 * @param fd 目标 socket
 * @param connId 调用方上次持有的连接 id
 * @return 同时匹配 fd 和 connId 的 Connection*；否则 nullptr
 *
 * 【通俗解释】
 * fd 会被内核复用——某 fd 旧连接关闭后，新连接可能复用同一 fd。如果调用方拿着旧 connId
 * 想操作旧连接，但 fd 已被新连接占用，光按 fd 查会误把新连接当旧的。多查一次 connId
 * 即可识别"fd 复用但已是不同连接"并返回 nullptr，避免误操作。
 */
Connection *SubReactor::findConnection(int fd, uint64_t connId)
{
    auto *conn = findConnection(fd);
    if (!conn || conn->id != connId)
        return nullptr;
    return conn;
}

/**
 * @brief 标记连接"刚有活动"（心跳/PONG 校准用）
 * @param fd 目标 socket
 *
 * 【通俗解释】
 * 收到客户端任何数据（尤其是 PONG）时调用：①刷新 lastActiveSec 到当前秒；
 * ②清掉 waitingPong 标记（说明对端还活着，不再等 PONG）；③refresh 时间轮，
 * 把该 fd 从超时清单里"续命"。WebSocket 心跳保活的关键调用。
 */
void SubReactor::touchActivity(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    auto &t = it->second->timer;
    t.lastActiveSec = nowSec();
    t.waitingPong = false;
    wheel.refresh(fd);
}

/**
 * @brief 单纯续命时间轮（不更新心跳状态）
 * @param fd 目标 socket
 *
 * 【通俗解释】
 * 只 refresh 时间轮，不动 timer 的 lastActiveSec/waitingPong。用于普通 I/O 活动续命，
 * 与 touchActivity 区别在于：touchActivity 是心跳路径专用（会重置 PONG 等待状态），
 * refreshIdleTimer 是任意 I/O 路径都能调的轻量续命。
 */
void SubReactor::refreshIdleTimer(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    wheel.refresh(fd);
}

/**
 * @brief 本线程内入队一个出站任务
 * @param fd 目标 socket
 * @param task 出站任务（待发送的字节流 + 控制信息）
 * @return Ok 入队成功；Backpressure 水位已满；Closed 连接已不存在
 *
 * 【通俗解释】
 * Session 在本 Reactor 线程内调用：经 findConnection 拿到 conn，再委托给
 * outbound_.enqueue 把任务塞进该连接的 outboundQueue。出站路径的"快通道"——
 * 跨线程场景请改走 postOutbound（保证线程安全）。
 */
EnqueueResult SubReactor::enqueueOutbound(int fd, OutboundTask task)
{
    auto *conn = findConnection(fd);
    if (!conn)
        return EnqueueResult::Closed;
    return outbound_.enqueue(*conn, std::move(task));
}

/**
 * @brief 预约一个出站发送序号（ticket）
 * @param fd 目标 socket
 * @return ticket 序号；连接不存在返回 0
 *
 * 【通俗解释】
 * Session 想知道"我提交的这批出站任务何时被真正发完"时，先预约一个 ticket，
 * 把它和 OutboundTask 一起 enqueue；writerLoop 冲刷完成后会把 completedTicket
 * 推进到该 ticket，届时通过 wakeCoroutine(...AwaitType::SENT) 唤醒等待 SENT 的协程。
 * 这是 SendCompletionAwaiter 的配套机制。
 */
uint64_t SubReactor::reserveOutboundTicket(int fd)
{
    auto *conn = findConnection(fd);
    if (!conn)
        return 0;
    return outbound_.reserveTicket(*conn);
}

/**
 * @brief 写协程主循环：持续冲刷本连接的出站队列
 * @param fd 目标 socket
 * @param connId 连接 id（防 fd 复用误判）
 * @return Task<void> 协程任务
 *
 * 【通俗解释】
 * 每条连接启动时同时拉起一个 Writer 协程跑这个 loop：
 *   ①fd 已不在表/connId 不匹配/closed → co_return 退出；
 *   ②outboundQueue 空 → co_await OutboundAwaiter 挂起，等 enqueueOutbound/postOutbound
 *     把任务塞进来后再唤醒；
 *   ③调 transportWriter.flush 把队列里的字节真正 write 到 socket；
 *   ④flush 出错 → fd_close 关连接并退出；
 *   ⑤waitingTicket 已被 completedTicket 追上 → 唤醒等 SENT 的主协程；
 *   ⑥flush 返回 Blocked（内核缓冲满）或 Yielded（本轮公平预算用完）
 *     → co_await TransportWriteAwaiter，重新武装 EPOLLOUT 后把线程还给事件循环。
 *   关键设计：写协程与主协程分离，写阻塞不会卡住主协程的读/解析路径。
 */
Task<void> SubReactor::writerLoop(int fd, uint64_t connId)
{
    while (true)
    {
        auto it = conns.find(fd);
        if (it == conns.end() || it->second->id != connId ||
            it->second->state.closed)
            co_return;

        auto &conn = *it->second;
        if (conn.transport.outboundQueue.empty())
        {
            co_await OutboundAwaiter(this, ConnectionKey{fd, connId});
            continue;
        }

        const auto result = transportWriter.flush(conn);
        if (result.status == FlushStatus::Error)
        {
            transportWriter.cancelAll(conn, OutboundOutcome::WriteError);
            fd_close(fd, "transport writer error", CoroutineRole::Writer);
            co_return;
        }

        if (conn.transport.waitingTicket != 0 &&
            conn.transport.completedTicket >= conn.transport.waitingTicket)
            wakeCoroutine(fd, CoroutineRole::Main, AwaitType::SENT);

        updateEvent(fd);
        if (result.closeRequested)
        {
            fd_close(fd, "outbound close completed", CoroutineRole::Writer);
            co_return;
        }
        if (result.status == FlushStatus::Blocked ||
            result.status == FlushStatus::Yielded)
            co_await TransportWriteAwaiter(this, ConnectionKey{fd, connId});
    }
}

/**
 * @brief 跨线程投递出站任务（线程安全）
 * @param fd 目标 socket
 * @param connId 连接 id（消费时校验）
 * @param task 出站任务
 *
 * 【通俗解释】
 * Worker 线程或别的 Reactor 想给本连接发数据时调用：
 *   ①本 Reactor 已停 → 直接丢弃；
 *   ②当前已在本 Reactor 线程内 → 直接 enqueueOutbound，省 eventfd 往返；
 *   ③跨线程 → 委托 outbound_.post 把任务先存进 pending 队列，再写 eventfd 唤醒
 *     本 Reactor 线程，由 processPendingOutbound 消费 pending 并 enqueue 到
 *     各连接的 outboundQueue。这样所有对 outboundQueue 的写操作都集中在
 *     本 Reactor 线程，无需加锁。
 * @return 当前一级的准入结果；Ok 不代表 socket 已经写完
 */
EnqueueResult SubReactor::postOutbound(int fd,
                                       uint64_t connId,
                                       OutboundTask task)
{
    if (!running.load(std::memory_order_acquire))
    {
        task.complete(OutboundOutcome::Closed);
        return EnqueueResult::Closed;
    }

    // 本 Reactor 线程内直接入队，避免 eventfd 往返
    if (th.joinable() && th.get_id() == std::this_thread::get_id())
    {
        if (auto *conn = findConnection(fd, connId))
            return outbound_.enqueue(*conn, std::move(task));
        task.complete(OutboundOutcome::Stale);
        return EnqueueResult::Closed;
    }

    return outbound_.post(fd, connId, std::move(task), event_fd, notified);
}

/**
 * @brief 消费跨线程积压的出站任务（仅本 Reactor 线程调用）
 *
 * 【通俗解释】
 * loop 处理 eventfd 唤醒后调用：把 outbound_.pending 队列里别的线程 post 过来的
 * 任务挨个 swap 出来，按 (fd, connId) 路由到对应连接的 outboundQueue。connId 不匹配
 * 的旧任务会被丢弃（fd 已被复用）。处理完后 writerLoop 协程会自然被 OutboundAwaiter
 * 唤醒来冲刷这些新入队的数据。
 */
void SubReactor::processPendingOutbound()
{
    const auto stats = outbound_.processPending(conns);
    if (stats.backpressured != 0)
    {
        LOG_INFO(
            "outbound pending drain dropped " +
            std::to_string(stats.backpressured) +
            " task(s) because target connections were backpressured");
    }
}

/**
 * @brief 时间轮超时回调：决定是否真的关闭连接
 * @param fd 超时的 socket
 *
 * 【通俗解释】
 * 时间轮 tick 到某 fd 超时了调这个回调：
 *   ①连接已不在表 → 直接返回（可能已被别处关掉）；
 *   ②询问 session->onTimeout：返回 false 表示 session 想保留连接（例如 HTTP keep-alive
 *     的空闲超时可被新请求重置），那就 return 不关；
 *   ③session 同意关 → 区分原因：wsHeartbeat 且 waitingPong（心跳 PONG 等不到）→
 *     "heartbeat timeout"；普通空闲 → "timeout"，再调 fd_close 真正关闭。
 */
void SubReactor::onWheelTimeout(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;

    auto &conn = *it->second;
    if (conn.session && !conn.session->onTimeout())
        return;

    const char *reason =
        (conn.timer.wsHeartbeat && conn.timer.waitingPong) ? "heartbeat timeout"
                                                           : "timeout";
    fd_close(fd, reason);
}
