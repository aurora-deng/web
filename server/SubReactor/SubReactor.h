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
#include <mutex>
#include <string.h>
#include <algorithm>
#include <sys/sendfile.h>
#include <utility>

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
#define MAX_EVENTS 1024
#define KB(x) ((x) * 1024UL)
#define MB(x) ((x) * 1024UL * 1024UL)
#define MAX_PENDING_BYTES MB(1)       // 背压修复处：readBuffer 可读数据水位线，超过则暂停读
#define MAX_PIPELINE 256              // 背压修复处：最大并发任务数，从 1024 降为 256
#define MAX_PENDING_RESPONSES 512     // 背压修复处：pendingResponses 水位线，超过则暂停读
#define MAX_WRITE_BUFFER_BYTES MB(2)  // 背压修复处：写缓冲区总大小水位线
extern ThreadPool pool;         // 公用main函数全局的线程池
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

struct PendingRequest
{
    uint64_t seq;
    HttpRequest data;
};

struct pendingResponse
{
    size_t headerOffset = 0;
    std::string header;
    RespBodyPtr body;
};

// ---------------------------------------------------链接体（分层拆分）--------------------------

// 传输层：负责底层 I/O 状态和缓冲区
struct ConnTransport
{
    int fd = -1;
    uint64_t id = 0;
    Buffer readBuffer;
    ConnState state;
    size_t pendingBytes = 0; // readBuffer 中未读数据字节数，用于背压判断
};

// 协议层：负责 HTTP 请求/响应的流水线管理
struct ConnPipeline
{
    bool keepAlive = true;                              // 默认 keep-alive
    size_t inflightTasks = 0;                           // 正在线程池中处理的任务数
    uint64_t nextRequestSeq = 0;                        // 请求编号生成器
    uint64_t nextResponseSeq = 0;                       // 响应编号消费器
    std::queue<PendingRequest> pendingRequests;         // 待提交的请求队列
    std::map<uint64_t, pendingResponse> pendingResponses; // 待发送的响应队列（按 seq 排序）
};

// 定时器层：负责连接超时管理
struct ConnTimer
{
    uint64_t expireSlot = 0; // 当前所在时间槽的位置
    bool inWheel = false;    // 防止重复加入时间轮
};

// 连接对象：组合三层
struct Connection
{
    ConnTransport transport; // 传输层
    ConnPipeline pipeline;   // 协议层
    ConnTimer timer;         // 定时器层

    // 便捷访问方法（引用成员指向自己的子对象）
    int &fd = transport.fd;
    uint64_t &id = transport.id;
    Buffer &readBuffer = transport.readBuffer;
    ConnState &state = transport.state;
    size_t &pendingBytes = transport.pendingBytes;

    bool &keepAlive = pipeline.keepAlive;
    size_t &inflightTasks = pipeline.inflightTasks;
    uint64_t &nextRequestSeq = pipeline.nextRequestSeq;
    uint64_t &nextResponseSeq = pipeline.nextResponseSeq;
    std::queue<PendingRequest> &pendingRequests = pipeline.pendingRequests;
    std::map<uint64_t, pendingResponse> &pendingResponses = pipeline.pendingResponses;

    uint64_t &expireSlot = timer.expireSlot;
    bool &inWheel = timer.inWheel;

    // 引用成员导致默认拷贝/移动赋值被删除，需要自定义
    // 只拷贝子对象数据，引用自动指向自己的子对象
    Connection() = default;
    Connection(const Connection &other)
        : transport(other.transport), pipeline(other.pipeline), timer(other.timer),
          fd(transport.fd), id(transport.id), readBuffer(transport.readBuffer),
          state(transport.state), pendingBytes(transport.pendingBytes),
          keepAlive(pipeline.keepAlive), inflightTasks(pipeline.inflightTasks),
          nextRequestSeq(pipeline.nextRequestSeq), nextResponseSeq(pipeline.nextResponseSeq),
          pendingRequests(pipeline.pendingRequests), pendingResponses(pipeline.pendingResponses),
          expireSlot(timer.expireSlot), inWheel(timer.inWheel) {}
    Connection &operator=(const Connection &other)
    {
        if (this != &other)
        {
            transport = other.transport;
            pipeline = other.pipeline;
            timer = other.timer;
            // 引用已绑定到自己的子对象，无需重新赋值
        }
        return *this;
    }
    Connection(Connection &&other) noexcept
        : transport(std::move(other.transport)), pipeline(std::move(other.pipeline)), timer(std::move(other.timer)),
          fd(transport.fd), id(transport.id), readBuffer(transport.readBuffer),
          state(transport.state), pendingBytes(transport.pendingBytes),
          keepAlive(pipeline.keepAlive), inflightTasks(pipeline.inflightTasks),
          nextRequestSeq(pipeline.nextRequestSeq), nextResponseSeq(pipeline.nextResponseSeq),
          pendingRequests(pipeline.pendingRequests), pendingResponses(pipeline.pendingResponses),
          expireSlot(timer.expireSlot), inWheel(timer.inWheel) {}
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

struct TaskResult
{
    bool keepAlive = false;

    int fd = -1;
    uint64_t id = 0;  // fd链接id
    uint64_t seq = 0; // 用于当做响应请求时候的id

    std::string header;
    RespBodyPtr body;

    ConnState state;
};

// 用于唤醒stream使用的结构体
struct StreamNotify
{
    int fd;
    uint64_t connId;
};
// 使用结构联合体来控制内存使用,用于统一queue
using ReactorTask = std::variant<TaskResult, StreamNotify>;

// ----------------------------实体类-------------------------------------

class SubReactor
{
public:
    int epfd;
    std::unordered_map<int, Connection> conns; // 接受发送类
    std::thread th;
    int event_fd;
    size_t slotNum = 60; // 时间槽数量
    int timeout = 30;    // 超时时间

    // eventfd 优化处：使用 atomic<bool> + exchange 合并唤醒
    // 原代码每次 pushResult/notifyStream/addFd 都写 eventfd
    // 10000 个任务结果 = 10000 次 write(event_fd) 系统调用
    // 优化后：只有第一个写入者真正写 eventfd，后续写入者只入队不写
    // 10000 个任务结果 = 1 次 write(event_fd) + 9999 次 atomic exchange
    std::atomic<bool> notified{false};

    // uint64_t currentTick=0;         //记录全局时间
    // 将worker线程中的response加入到队列里面去
    std::mutex queue_mtx;
    // std::queue<TaskResult> push_to_SubReactor_queue;
    Router &router;
    // std::queue<StreamNotify> StreamNotifyQueue;     //用于处理流式id和fd
    std::queue<ReactorTask> Task_Queue;
    // 段错误修复处：新增 pendingFds 队列，解决 addFd 线程安全问题
    std::queue<int> pendingFds;
    std::mutex pending_mtx;
    TimerWheel wheel;

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
    void pushResult(ReactorTask res);
    void notifyStream(int fd, uint64_t id);
    // 统一套接字关闭
    void fd_close(int fd, std::string reason);

    // 设置fd为非堵塞，对于新添加的fd都要使用
    void fd_unblock(int fd);

    // 重新唤醒、
    void rearm(int fd, uint32_t events);

    // 用于优化集成rearm,结构性优化，接纳允许同时write和read
    void updateEvent(int fd);

    // 使用response多态继承之后统一发送函数
    SendState sendBody(int fd, pendingResponse &resp);

    // 写入函数
    void handleWrite(int fd);
    bool handleTaskResultOnce(); // 优化：统一了stream和tasks
    // 读取函数
    void handleRead(int fd);
    bool processRequest(int fd);
    // 段错误修复处：处理待添加的 fd 队列，由 SubReactor 线程调用
    void processPendingFds();

    void loop();
    void addFd(int fd);
};

#endif