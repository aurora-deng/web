#ifndef SUBREACTOR_H
#define SUBREACTOR_H

#include <cstdint>
#include <string>
#include <iostream>
#include <stdlib.h>
#include <thread>
#include <unordered_map>
#include <queue>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <unistd.h>
#include <strings.h>
#include <sys/socket.h>
#include <variant>
#include <unordered_set>
#include <fcntl.h>
#include <atomic>
#include <string.h>
#include <algorithm>
#include <mutex>
#include <sys/sendfile.h>
#include <utility>
#include <coroutine>
#include <stdexcept>

#include "server/timer/TimeWheel.h"
#include "server/http/http.h"
#include "server/threadpoll/thread_pool.h"
#include "server/Route/Router.h"
#include "server/Buffer/Buffer.h"
#include "server/BufferPoll/BufferPoll.h"
#include "server/Repsonse/RespBody.h"
#include "server/Repsonse/StringBody.h"
#include "server/Repsonse/ChunkedBody.h"
#include "server/Repsonse/FileBody.h"
#include "server/SegmentPool/SegmentPool.h"
#include "server/Repsonse/HeaderBody.h"
#include "server/ObjectPool/ObjectPool.h"
#include "server/CoroutineScheduler/CoroutineScheduler.h"
#include "server/CoroutineScheduler/Task.h"
#include "server/CoroutineScheduler/AWaiter.h"
#include "server/http/HttpSession/HttpSession.h"
#include "server/http/HttpParser/HttpParser.h"
#include "server/http/HttpCodec/HttpCodec.h"
#include "server/http/ResponseSender/ResponseSender.h"
#include "server/Executor/Executor.h"

#define MAX_EVENTS 1024
#define KB(x) ((x) * 1024UL)
#define MB(x) ((x) * 1024UL * 1024UL)

#define MAX_PENDING_BYTES MB(1) // 背压修复处：readBuffer 可读数据水位线，超过则暂停读

extern ObjectPoll<HttpRequest> requestPool;
extern ObjectPoll<HttpResponse> responsePool;
// 优化，使用多reactor，每个reactor拥有独自的epoll，
// 并且每个reactor独自占领独自资源实现类似单独进程的作用，拥有自己的资源，从而实现无锁

// 使用状态机来处理发送返回结果
enum SendState;

enum class RecvState
{
    // READY 表示本轮已读到 EAGAIN，可继续解析；PAUSED 表示触发背压并已撤销读关注；
    // CLOSED 表示连接对象可能已删除，调用方不得继续持有旧引用。
    READY,
    PAUSED,
    CLOSED
};
// ------------------------任务线程处理中间体----------------------

// ---------------------------------------------------链接体（分层拆分）--------------------------

// 传输层：负责底层 I/O 状态和缓冲区
struct ConnTransport
{
    int fd = -1;
    uint64_t id = 0;
    Buffer readBuffer;
    ConnState state;
    size_t pendingBytes = 0; // 统计目前fd中已经储存的请求数据的总字节量，用于控制合适的时候拒绝read数据保持待机状态
};


// 定时器层：负责连接超时管理
struct ConnTimer
{
    uint64_t expireSlot = 0; // 记录当前所在时间槽的位置
    bool inWheel = false;    // 防止重复加入
};

struct Connection
{
    ConnTransport transport; // 传输层
    ConnTimer timer;         // 定时器层
    // session 由 Connection 持有，调度器在协程存活期间还会保留共享所有权；
    // 这样连接从 conns 删除后，已唤醒的协程仍可安全执行到 co_return，而不会访问已析构 session。
    std::shared_ptr<HttpSession> session;

    int &fd = transport.fd;
    uint64_t &id = transport.id;
    Buffer &readBuffer = transport.readBuffer;
    ConnState &state = transport.state;
    size_t &pendingBytes = transport.pendingBytes;


    uint64_t &expireSlot = timer.expireSlot;
    bool &inWheel = timer.inWheel;


    Connection() = default;
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&) = delete;
    Connection &operator=(Connection &&) = delete;
};
// --------------------------------任务接受体----------------------------

// 用于唤醒stream使用的结构体
// 流式响应的异步通知同时携带 fd 与连接代号；fd 会被内核复用，connId 可供后续扩展时
// 拒绝投递到“同 fd、不同连接”的旧通知。
struct StreamNotify
{
    int fd;
    uint64_t connId;
};

// ----------------------------实体类-------------------------------------

class SubReactor
{
public:
    // conns 及其指向对象只允许 SubReactor 线程访问；跨线程入口仅限 addFd 的受锁队列。
    int epfd;
    std::unordered_map<int, std::unique_ptr<Connection>> conns; // 接受发送类
    std::thread th;
    std::atomic<bool> running{true};        //使用原子化处理，来判断是否运行
    int event_fd;
    size_t slotNum = 60; // 时间槽数量
    int timeout = 30;    // 超时时间

    // notified 合并连续 eventfd 写入，避免高并发接入时为每个 fd 都触发一次系统调用。
    std::atomic<bool> notified{false};

    std::queue<int> pendingFds;
    std::mutex pending_mtx;
    TimerWheel wheel;
    SegmentPool segPool;
    ResponseSender sender;
    // Codec 与 Executor 由 ServerRuntime 统一管理；所有 SubReactor 只借用，
    // 避免每个 Reactor 各自创建一组 Worker 导致线程数平方级膨胀，所以使用地址引用。
    HttpCodec &codec;
    Executor &executor;

    // Worker 线程完成后通过 completeQueue 通知 Reactor 线程
    std::mutex completeMtx;
    std::queue<std::pair<int, uint64_t>> completeQueue;
    // 僵尸协程唤醒表：fd_close 时协程正在 Executor 中执行，连接被删除后
    // Worker 完成时通过 connId 在此查找 handle 并直接调度，防止协程泄漏。
    std::unordered_map<uint64_t, std::coroutine_handle<>> zombieWakes;
    
    // 调度器是协程句柄的统一排队和销毁点，避免 I/O 回调直接 resume/destroy 造成重入或悬空句柄。
    CoroutineScheduler scheduler;

    SubReactor(HttpCodec &codec, Executor &executor)
        : wheel(slotNum, timeout),
          sender(segPool, wheel),
          codec(codec),
          executor(executor)
    {
        // eventfd 把主线程投递转换为本 Reactor 的普通可读事件，使连接注册仍在所有者线程执行。
        epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd == -1)
            throw std::runtime_error(std::string("subreactor epoll_create1: ") + strerror(errno));

        // 用于实现多线程之间通信 + 唤醒 epoll
        event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

        if (event_fd == -1)
        {
            close(epfd);
            throw std::runtime_error(std::string("subreactor eventfd: ") + strerror(errno));
        }
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = event_fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &ev) == -1)
        {
            close(event_fd);
            close(epfd);
            throw std::runtime_error(std::string("subreactor epoll_ctl: ") + strerror(errno));
        }

        wheel.setCloseCallbace(
            [this](int fd)
            {
                fd_close(fd, "timeout");
            });
        // 协程完成后只在句柄仍匹配当前 session 时清理上下文；地址比对可避免旧完成通知
        // 清掉后续协程登记的句柄，为未来支持协程替换/重启保留安全边界。
        scheduler.setCompletionCallback(
            [this](int fd, std::coroutine_handle<> handle)
            {
                auto it = conns.find(fd);
                if (it == conns.end() || !it->second->session)
                    return;
                auto &ctx = it->second->session->coroutine_context;
                if (ctx.handle && ctx.handle.address() == handle.address())
                {
                    ctx.handle = nullptr;
                    ctx.state = AwaitType::NONE;
                    ctx.waiting = false;
                }
            });
    }
    ~SubReactor();
    void run();
    void stop();
    void join();
    // 统一套接字关闭
    void fd_close(int fd, std::string_view reason, bool fromCoroutine = false);

    // 设置fd为非堵塞，对于新添加的fd都要使用
    void fd_unblock(int fd);

    // 重新唤醒、
    void rearm(int fd, uint32_t events);
    // 用于优化集成rearm,结构性优化，接纳允许同时write和read
    void updateEvent(int fd);
    // 使用 wakeReadCoroutine/wakeWriteCoroutine 统一进入 scheduler.schedule 去重。
    // 读取函数
    void wakeReadCoroutine(int fd);
    // 写入函数
    void wakeWriteCoroutine(int fd);
    // 唤醒执行完成的协程（由 processComplete 调用）
    void wakeExecuteCoroutine(int fd);
    // Worker 线程完成 handler 后调用（线程安全），投递完成通知
    void notifyExecuteComplete(int fd, uint64_t connId);
    // Reactor 线程消费完成队列，唤醒对应协程
    void processComplete();

    RecvState recvSocket(int fd);
    // 段错误修复处：处理待添加的 fd 队列，由 SubReactor 线程调用
    void processPendingFds();

    void loop();
    void addFd(int fd);
};

#endif