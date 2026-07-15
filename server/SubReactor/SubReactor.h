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
#include "server/http/HttpSession.h"

#define MAX_EVENTS 1024
#define KB(x) ((x) * 1024UL)
#define MB(x) ((x) * 1024UL * 1024UL)

#define MAX_PENDING_BYTES MB(1)      // 背压修复处：readBuffer 可读数据水位线，超过则暂停读
#define MAX_PIPELINE 256             // 背压修复处：最大并发任务数，从 1024 降为 256
#define MAX_PENDING_RESPONSES 512    // 背压修复处：pendingResponses 水位线，超过则暂停读
#define MAX_WRITE_BUFFER_BYTES MB(2) // 背压修复处：写缓冲区总大小水位线

extern ObjectPoll<HttpRequest> requestPool;
extern ObjectPoll<HttpResponse> responsePool;
// 优化，使用多reactor，每个reactor拥有独自的epoll，
// 并且每个reactor独自占领独自资源实现类似单独进程的作用，拥有自己的资源，从而实现无锁

// 使用状态机来处理发送返回结果
enum SendState
{
    SEND_OK,
    SEND_AGAIN,
    SEND_Writev_CLOSED,
    SEND_Chunk_CLOSED,
    SEND_Header_CLOSED,
    SEND_EndChunk_CLOSED,
    SEND_File_CLOSED,
    SEND_REMAIN
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

// 协议层：负责 HTTP 请求/响应的流水线管理
struct ConnPipeline
{
    bool keepAlive = true;
                                                          //  用于高并发下回复一致性
    uint64_t nextRequestSeq = 0;                          // 生成请求编号
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
    ConnPipeline pipeline;   // 协议层
    ConnTimer timer;         // 定时器层
    // 保存当前连接的对应的业务状态,用来保存业务的所有信息
    std::shared_ptr<HttpSession> session;

    int &fd = transport.fd;
    uint64_t &id = transport.id;
    Buffer &readBuffer = transport.readBuffer;
    ConnState &state = transport.state;
    size_t &pendingBytes = transport.pendingBytes;

    bool &keepAlive = pipeline.keepAlive;
    uint64_t &nextRequestSeq = pipeline.nextRequestSeq;

    uint64_t &expireSlot = timer.expireSlot;
    bool &inWheel = timer.inWheel;

   
    Connection() = default;
    // 拷贝构造函数 Connection(const Connection &other)
    Connection(const Connection &other) : transport(other.transport), pipeline(other.pipeline), timer(other.timer),
                                          fd(transport.fd), id(transport.id), readBuffer(transport.readBuffer),
                                          state(transport.state), pendingBytes(transport.pendingBytes),
                                          keepAlive(pipeline.keepAlive), 
                                          nextRequestSeq(pipeline.nextRequestSeq),
                                          expireSlot(timer.expireSlot), inWheel(timer.inWheel) {}

    // 拷贝赋值运算符 operator=(const Connection &other)
    Connection &operator=(const Connection &other)
    {
        if (this != &other)
        {
            transport = other.transport;
            pipeline = other.pipeline;
            timer = other.timer;
        }
        return *this;
    }

    // 移动构造函数 Connection(Connection &&other) noexcept
    Connection(Connection &&other) noexcept : transport(std::move(other.transport)), pipeline(std::move(other.pipeline)), timer(std::move(other.timer)),
                                              fd(transport.fd), id(transport.id), readBuffer(transport.readBuffer),
                                              state(transport.state), pendingBytes(transport.pendingBytes),
                                              keepAlive(pipeline.keepAlive),
                                              nextRequestSeq(pipeline.nextRequestSeq),
                                              expireSlot(timer.expireSlot), inWheel(timer.inWheel), session(std::move(other.session)) {}
    // 移动赋值运算符 operator=(Connection &&other) noexcept
    Connection &operator=(Connection &&other) noexcept
    {
        if (this != &other)
        {
            transport = std::move(other.transport);
            pipeline = std::move(other.pipeline);
            timer = std::move(other.timer);
        }
        return *this;
    }
};
// --------------------------------任务接受体----------------------------

// 用于唤醒stream使用的结构体
struct StreamNotify
{
    int fd;
    uint64_t connId;
};

// ----------------------------实体类-------------------------------------

class SubReactor
{
public:
    int epfd;
    std::unordered_map<int, std::unique_ptr<Connection>> conns; // 接受发送类
    std::thread th;
    int event_fd;
    size_t slotNum = 60; // 时间槽数量
    int timeout = 30;    // 超时时间

   
    std::atomic<bool> notified{false};

    Router &router;

    std::queue<int> pendingFds;
    std::mutex pending_mtx;
    TimerWheel wheel;
    SegmentPool segPool;

    // 协程对象
    CoroutineScheduler scheduler;

    SubReactor(Router &router) : router(router), wheel(slotNum, timeout)
    {
        // 创建属于自己的epoll
        epfd = epoll_create(1);
        // 用于实现多线程之间通信 + 唤醒 epoll
        event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = event_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &ev);
        wheel.setCloseCallbace(
            [this](int fd)
            {
                fd_close(fd, "timeout");
            });
    }
    void run();
    // 统一套接字关闭
    void fd_close(int fd, std::string reason, bool fromCoroutine = false);

    // 设置fd为非堵塞，对于新添加的fd都要使用
    void fd_unblock(int fd);

    // 重新唤醒、
    void rearm(int fd, uint32_t events);
    // 用于优化集成rearm,结构性优化，接纳允许同时write和read
    void updateEvent(int fd);
    // 使用 wakeReadCoroutine,wakeWriteCoroutine 防止重复 scheduler.add
    // 读取函数
    void wakeReadCoroutine(int fd);
    // 写入函数
    void wakeWriteCoroutine(int fd);
    // 使用response多态继承之后统一发送函数
    SendState sendBody(int fd, HttpResponse &resp);

    HttpResponse *createResponse(HttpRequest &req);
    void finishReaponse();
    bool recvSocket(int fd);
    bool parseOneRequest(HttpRequest &req, Connection &conn);
    // 段错误修复处：处理待添加的 fd 队列，由 SubReactor 线程调用
    void processPendingFds();

    void loop();
    void addFd(int fd);
};

#endif