// =============================================================================
// 文件名：SubReactor.h
// 所属模块：server/SubReactor —— 从 Reactor（单线程事件循环 + 协程调度编排层）
//
// 【职责比喻：车间主任 / 护士长 / 楼层经理】
// SubReactor 是"从 Reactor"——单线程事件循环，独自管理一批连接的 I/O 读写、
// 协程调度、定时器超时、出站冲刷。在整个系统里它像一个车间主任（也是护士长、楼层经理），
// 独自看管本"车间"的所有客房（连接）：
//   ①接客人入住（addFd→注册 epoll）；②听客人发话（EPOLLIN→wakeReadCoroutine）；
//   ③回客人话（EPOLLOUT→wakeWriteCoroutine）；④给客人设超时（TimerWheel，到点没动作就请走）；
//   ⑤把重活外包给后厨（Executor，业务 handler），后厨做完按铃通知（eventfd→processComplete）。
// 一位车间主任只服务自己车间的客人，跟别的车间主任不抢生意——所以本类成员基本无需加锁，
// 只有跨线程入口（addFd / notifyExecuteComplete）需要锁保护队列。
//
// 【第四阶段重构动机：依赖倒置，协议相关细节下沉到 Session/SessionFactory】
// 第四阶段之前，SubReactor 持有共享的 HttpCodec——那时只有 HTTP 一种协议，所有连接
// 共享同一套编解码逻辑没问题。第四阶段引入 WebSocket 后，不同连接可能是 HTTP 或
// WebSocket 两种协议，共享一个 codec 会串扰。故重构为：
//   - codec 下沉到每个 Session 内部：HttpSession 自带 HttpCodec，
//     WebSocketSession 自带 WebSocketCodec，每条连接独立编解码互不干扰；
//   - SubReactor 不再持有 codec，改成持有 Router& —— 路由器对 HTTP/WS 都通用，协议无关；
//   - 出站发送不在 Session 抽象 sender，统一走 OutboundTask + OutboundQueue +
//     TransportWriter + writerLoop 体系；
//   - 协议升级（HTTP→WebSocket）所需的子类创建通过 SessionFactory 注入，本类只持有
//     SessionFactory* 抽象指针（sessionFactory_），不直接依赖 WebSocket 具体类型——
//     这是依赖倒置的体现。SessionFactory 由 ServerRuntime 创建，经 ReactorGroup
//     注入到每个 SubReactor（setSessionFactory）。
//
// 关键技术点（初学者重点理解）：
// 1. 【单线程事件循环】一个 SubReactor 一条线程，epoll_wait 等事件，事件来了唤醒
//    对应协程，协程挂起/恢复都通过 CoroutineScheduler 统一调度。这样避免了多线程
//    抢一把锁的竞争，I/O 性能极高。
// 2. 【EPOLLONESHOT】每个 fd 武装后只触发一次，触发后必须 rearm 才能再次监听。
//    这避免了同一 fd 的事件被多次并发唤醒，保证"同一连接同一时刻只有一个协程在跑"。
// 3. 【eventfd 跨线程唤醒】主线程（addFd）和 Worker 线程（notifyExecuteComplete）
//    通过 eventfd 把"有事要做"通知本 Reactor 线程，让所有 epoll/conns 操作集中在
//    SubReactor 自己的线程做，消除数据竞争。
// 4. 【findConnection 唯一查表入口】conns 表只允许本 Reactor 线程访问；外部
//    （Session/Awaiter）需要查连接一律走 findConnection(fd) / findConnection(fd, connId)，
//    不得直接摸 conns。带 connId 的重载用于识别"fd 复用但已是不同连接"的旧引用。
// 5. 【出站走 OutboundQueue】出站路径用独立的 OutboundQueue 类（不是 std::queue），
//    关键方法：enqueueOutbound（本线程入队）/ postOutbound（跨线程投递）/
//    reserveOutboundTicket（预约发送序号）/ writerLoop（协程冲刷 TransportWriter）/
//    processPendingOutbound（消费跨线程积压）。OutboundQueue 内部维护每连接的
//    outboundQueue + ticket 机制，支持发送完成通知（SendCompletionAwaiter）。
// 6. 【SessionFactory 依赖倒置】协议升级时由 sessionFactory_ 创建 WebSocketSession
//    等子类，本类只持有 SessionFactory* 抽象指针，不直接依赖 WebSocket 具体类型。
//    SessionFactory 由 ServerRuntime 创建并经 ReactorGroup 注入到每个 SubReactor。
// 7. 【时间轮 TimerWheel】每个 fd 加进时间轮，收到数据就 refresh；超时未活动
//    的 fd 由时间轮回调 onWheelTimeout→fd_close 关闭，防止僵尸连接占资源。
// 8. 【协程+线程池分工】协程跑 I/O 路径（recv/send/co_await），重计算的业务 handler
//    投递到 Executor 线程池跑，跑完通过 completeQueue 通知本 Reactor 唤醒协程续发响应。
// =============================================================================
#ifndef SUBREACTOR_H
#define SUBREACTOR_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <coroutine>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include "server/CoroutineScheduler/CoroutineScheduler.h"
#include "server/CoroutineScheduler/Task.h"
#include "server/SegmentPool/SegmentPool.h"
#include "server/timer/TimeWheel.h"
#include "server/transport/Connection.h"
#include "server/transport/OutboundQueue.h"
#include "server/transport/TransportWriter.h"

class Executor;
class Router;
class SessionFactory;
class TlsContext;

inline constexpr int kMaxEvents = 1024;

/**
 * @brief 接收数据后的状态
 *
 * 【通俗解释】
 * recvSocket 调用后告诉调用方"刚才 recv 的情况如何"：
 *   READY   - 数据已读完（遇到 EAGAIN），可以继续解析
 *   PAUSED  - 触发背压（数据太多），已暂停读关注，等积压消化后再恢复
 *   CLOSED  - 连接已死，conns 表里可能已删除，调用方不得再持有旧引用
 */
enum class RecvState
{
    READY,    // 数据已读到 EAGAIN，可继续解析
    PAUSED,   // 触发背压，已撤销读关注；待积压消化后由 updateEvent 恢复
    CLOSED    // 连接对象可能已删除，调用方不得继续持有旧引用
};

/**
 * @brief 从 Reactor：协议无关的 I/O 事件层 + 生命周期编排
 *
 * 【通俗解释】
 * 酒店某楼层的"楼层经理"（车间主任/护士长）：一人独管本楼层的所有客房（连接），自带一个
 * epoll（监控台）、一个时间轮（活动计时器）、一个调度器（服务员排班表）、
 * 一个 eventfd（跨线程叫号铃）、一个 completeQueue（后厨出餐窗口）、一个 OutboundQueue
 * （出站发送队列）。跨线程入口只有 addFd（接客）和 notifyExecuteComplete（后厨按铃），
 * 都通过锁队列+eventfd 把工作转交给本 Reactor 自己的线程处理，消除数据竞争。
 *
 * 公开面按权责收窄：查表只用 findConnection；出站只用 enqueueOutbound/postOutbound/
 * reserveOutboundTicket；loop、process、wake 等实现细节全部 private。
 *
 * @note 第四阶段重构后，本类不再持有协议相关的 codec/sender，只持有协议无关的 Router&
 *       与 SessionFactory*。协议编解码由 Session 子类自持；协议升级由 SessionFactory
 *       创建子类；出站发送由 OutboundQueue + TransportWriter + writerLoop 承担。
 */
class SubReactor
{
public:
    /**
     * @brief 构造函数：创建 epoll、eventfd，注册事件回调
     * @param router 路由表引用（协议无关，由 ServerRuntime 持有，所有 SubReactor 共享）
     * @param executor 业务执行器引用（线程池封装，由 ServerRuntime 持有，所有 SubReactor 共享）
     *
     * 【通俗解释】
     * 楼层经理上任：①开监控台（epoll_create1）；②装叫号铃（eventfd）并注册到监控台；
     * ③给时间轮装回调（超时→onWheelTimeout 请走客人）；④给调度器装完成回调
     * （服务员干完→清理 conns 里的句柄引用）。sessionFactory_ 此处还为空，由
     * ReactorGroup::start 后续调 setSessionFactory 注入。
     *
     * @note 第四阶段前参数为 HttpCodec&；为支持多协议改成 Router& —— 路由器对 HTTP/WS
     *       都通用，协议相关 codec 已下沉到各 Session 子类内部，本类无需持有 codec。
     */
    SubReactor(Router &router, Executor &executor);
    ~SubReactor();

    /** @brief 启动事件循环线程（开一条新线程跑 loop()） */
    void run();
    /** @brief 通知事件循环线程停止（置 running=false 并写 eventfd 唤醒） */
    void stop();
    /** @brief 阻塞等待事件循环线程退出（join 内部线程） */
    void join();

    /** @return 当前活跃连接数（原子读取，可跨线程） */
    size_t activeConnections() const
    {
        return activeConns_.load(std::memory_order_relaxed);
    }

    /** @brief 注入 SessionFactory（由 ReactorGroup 在 start 时调用），供协议升级时创建子类 Session */
    void setSessionFactory(SessionFactory *factory) { sessionFactory_ = factory; }
    /** @return 当前注入的 SessionFactory 指针（可能为空，未注入时） */
    SessionFactory *sessionFactory() const { return sessionFactory_; }
    /** @brief 设置本 Reactor 在 ReactorGroup 中的下标，供跨 Reactor 路由出站任务使用 */
    void setReactorIndex(size_t index) { reactorIndex_ = index; }
    /** @return 本 Reactor 在 ReactorGroup 中的下标 */
    size_t reactorIndex() const { return reactorIndex_; }

    /** @brief 连接表唯一查表入口（仅 Reactor 线程调用），外部 Session/Awaiter 不得直接摸 conns */
    Connection *findConnection(int fd);
    /** @brief 带 connId 的查表重载：fd 复用但 connId 不匹配时返回 nullptr，防旧引用误用 */
    Connection *findConnection(int fd, uint64_t connId);

    /** @return 协程调度器引用（供 Awaiter 注册/恢复协程句柄） */
    CoroutineScheduler &scheduler() { return scheduler_; }
    /** @return 业务执行器引用（供 Session 把耗时 handler 投递到线程池） */
    Executor &executor() { return executor_; }
    /** @return 路由表引用（供 HttpSession 构造 HttpCodec 时按 URL 分发） */
    Router &router() { return router_; }

    /**
     * @brief 跨线程入口：把新 fd 投递到本 Reactor（线程安全）
     *
     * 【通俗解释】前台把客人房号送过来：设 fd 非阻塞 → 加锁 push 进 pendingFds →
     * 用 atomic exchange 合并 eventfd 写入只唤醒一次本 Reactor 线程。真正的
     * epoll 注册与 Connection 创建由本线程在 processPendingFds 里完成，避免数据竞争。
     */
    // acceptor 把监听来源一并传入；TLS fd 会先创建 TlsSession 做握手。
    void addFd(int fd, bool tls = false);
    void setTlsContext(std::shared_ptr<TlsContext> context) { tlsContext_ = std::move(context); }

    /**
     * @brief 统一套接字关闭流程（仅本 Reactor 线程调用）
     * @param fd 要关闭的 socket
     * @param reason 关闭原因（日志用）
     * @param initiator 触发关闭的协程角色（initiator 自身不会被再次唤醒，防重入）
     */
    void fd_close(int fd,
                  std::string_view reason,
                  CoroutineRole initiator = CoroutineRole::Count);
    /** @brief 用 EPOLLONESHOT 重新武装 fd 的事件监听 */
    void rearm(int fd, uint32_t events);
    /** @brief 综合背压与写关注，重新计算并 rearm fd 的 epoll 事件掩码 */
    void updateEvent(int fd);
    /**
     * @brief 从 socket 读取数据到 readBuffer，循环 recv 直到 EAGAIN
     * @return READY/PAUSED/CLOSED 三态
     */
    RecvState recvSocket(int fd);
    /** @brief 刷新连接活跃时间（收消息 / Pong 时调用），重置时间轮与心跳等待标记 */
    void touchActivity(int fd);
    /** @brief 仅刷新时间轮倒计时（不重置心跳标记），供发送等非读活动续期 */
    void refreshIdleTimer(int fd);

    /**
     * @brief 本线程入站出站任务：将 OutboundTask 入队到 OutboundQueue
     * @return Closed 表示连接已不在；其他见 EnqueueResult
     */
    EnqueueResult enqueueOutbound(int fd, OutboundTask task);
    /**
     * @brief 预约一个发送完成 ticket，供协程 co_await SendCompletionAwaiter 等待对端发完
     * @return 0 表示连接不存在；否则返回单调递增的 ticket 号
     */
    uint64_t reserveOutboundTicket(int fd);
    /**
     * @brief 跨线程出站投递：把发送任务交给本 Reactor 的 OutboundQueue
     *
     * 若调用方就在本 Reactor 线程，直接 enqueue 避免 eventfd 往返；
     * 否则调 outbound_.post 投递并经 eventfd 唤醒本线程在 processPendingOutbound 消费。
     * @return 当前一级的准入结果；Ok 仅表示进入本地连接队列或跨线程邮箱
     */
    EnqueueResult postOutbound(int fd, uint64_t connId, OutboundTask task);

    /**
     * @brief 出站冲刷协程：循环把 OutboundQueue 里的任务经 TransportWriter 写到 socket
     *
     * 队列空时 co_await OutboundAwaiter 挂起；写阻塞时 co_await TransportWriteAwaiter 挂起；
     * 全部发完且 closeRequested 时调 fd_close 收尾。
     */
    Task<void> writerLoop(int fd, uint64_t connId);
    /**
     * @brief Worker 线程完成 handler 后调用（线程安全）：投递 (fd,connId) 到 completeQueue 并 eventfd 唤醒
     */
    void notifyExecuteComplete(int fd, uint64_t connId);

private:
    void loop();                  // 事件循环主体（本线程内运行）
    void processPendingFds();     // 批量办理 addFd 投递的待入驻 fd
    void processComplete();       // 消费 completeQueue，匹配 connId 唤醒协程或处理僵尸唤醒
    void processPendingOutbound();// 消费跨线程 postOutbound 投递的出站任务
    void fd_unblock(int fd);      // 把 fd 设为非阻塞
    void wakeReadCoroutine(int fd);   // 唤醒等待 READ 的协程
    void wakeWriteCoroutine(int fd);  // 唤醒等待 WRITE 的协程（Main + Writer 两个角色）
    void wakeExecuteCoroutine(int fd);// 唤醒等待 EXECUTE 完成的协程
    void wakeCoroutine(int fd, CoroutineRole role, AwaitType expected); // 统一唤醒入口，经 scheduler_.schedule 去重
    void onWheelTimeout(int fd);  // 时间轮超时回调：协商 Session 后调 fd_close
    static uint64_t nowSec();     // 当前单调时钟秒数
    static const char *roleName(CoroutineRole role); // 角色名转字符串（日志用）

    int epfd = -1;                // epoll 实例 fd（监控台）
    int event_fd = -1;            // eventfd：跨线程叫号铃，唤醒阻塞在 epoll_wait 的本线程
    std::unordered_map<int, std::unique_ptr<Connection>> conns; // fd→Connection 表（仅本线程访问）
    std::thread th;               // 本 Reactor 的事件循环线程
    std::atomic<bool> running{true};        // 运行标志，控制 loop 退出
    std::atomic<size_t> activeConns_{0};   // 活跃连接计数（原子，可跨线程读）
    // notified 合并连续 eventfd 写入，避免高并发接入时为每个 fd 都触发一次系统调用
    std::atomic<bool> notified{false};

    size_t slotNum = 60;          // 时间轮格子数
    int timeout = 30;             // 连接空闲超时秒数

    struct PendingFd { int fd; bool tls; };
    std::queue<PendingFd> pendingFds; // fd 与监听来源一起传递，不能靠连接字节猜 TLS
    std::mutex pending_mtx;       // 保护 pendingFds 的锁

    TimerWheel wheel;             // 时间轮：连接超时管理
    SegmentPool segPool;          // 发送分段对象池（Block 托盘回收站）
    TransportWriter transportWriter; // 真正写 socket 的组件（writev 批量发送）
    OutboundQueue outbound_;      // 出站任务队列（独立类，管理每连接 outboundQueue + ticket）

    Router &router_;              // 路由表引用（协议无关，HTTP/WS 共用）
    Executor &executor_;          // 业务执行器引用（线程池封装）
    size_t reactorIndex_ = 0;     // 本 Reactor 在 ReactorGroup 中的下标
    SessionFactory *sessionFactory_ = nullptr; // 协议升级工厂（依赖倒置，由 ReactorGroup 注入）
    std::shared_ptr<TlsContext> tlsContext_; // TLS 配置共享到连接，Reactor 关闭前有效

    std::mutex completeMtx;       // 保护 completeQueue 的锁
    std::queue<std::pair<int, uint64_t>> completeQueue; // Worker 完成通知队列（fd, connId）
    // 僵尸协程唤醒表：fd_close 时协程正在 Executor 中执行，连接被删除后
    // Worker 完成时通过 connId 在此查找 handle 并直接调度，防止协程泄漏。
    std::unordered_map<uint64_t, std::coroutine_handle<>> zombieWakes;

    // 调度器是协程句柄的统一排队和销毁点，避免 I/O 回调直接 resume/destroy 造成重入或悬空句柄。
    CoroutineScheduler scheduler_;
};

#endif
