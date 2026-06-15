#ifndef SUBREACTOR_H
#define SUBREACTOR_H

#include <cstdint>
#include <string>
#include <iostream>
#include <stdlib.h>
#include <thread>
#include <unordered_map>
#include<queue>
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
#include<mutex>
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
#define MAX_PENDING_BYTES MB(4) // 最大数据长
#define MAX_PIPELINE 1024       // 最大任务提交数
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
    // std::string data;
    // size_t offset = 0; // 优化：通过使用偏移量来判断数据是否发完，提效
    // 进一步优化，实现零拷贝,但是在发送之前需要进行常规和chunked判断

    size_t headerOffset = 0;
    std::string header;
    RespBodyPtr body;
};


// ---------------------------------------------------链接体--------------------------


struct Connection
{
    // 崩溃修复处：keepAlive 必须初始化为 true
    // 原代码未初始化，值为未定义。如果任务结果未被处理（keepAlive 未被设置），
    // handleWrite 检查 keepAlive 时读到垃圾值，可能为 false → 连接被错误关闭
    bool keepAlive=true;
    int fd;
    uint64_t id;
    size_t pendingBytes = 0;      // 统计目前fd中已经储存的请求数据的总字节量，用于控制合适的时候拒绝read数据保持待机状态
    size_t inflightTasks = 0;     // 表示限制任务处理的提交数量太多
                                  //  用于高并发下回复一致性
    uint64_t nextRequestSeq = 0;  // 生成请求编号
    uint64_t nextResponseSeq = 0; // 生成响应编号
    Buffer readBuffer;
    ConnState state;
    std::queue<PendingRequest> pendingRequests;           // 消息队列用于分离请求，缓解readBuffer即做缓冲队列又做请求处理队列
    std::map<uint64_t, pendingResponse> pendingResponses; // 用于当做回复消息的队列，同时，通过map精准对应请求seq_id
    // // 增加时间轮,创立时间戳
    // std::chrono::steady_clock::time_point lastActive;
    // 记录运行时间
    uint64_t ecpireSlot = 0; // 记录当前所在时间槽的位置
    bool inWheel = false;    // 防止重复加入
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

    // uint64_t currentTick=0;         //记录全局时间
    // 将worker线程中的response加入到队列里面去
    std::mutex queue_mtx;
    // std::queue<TaskResult> push_to_SubReactor_queue;
    Router &router;
    // std::queue<StreamNotify> StreamNotifyQueue;     //用于处理流式id和fd
    std::queue<ReactorTask> Task_Queue;
    // 段错误修复处：新增 pendingFds 队列，解决 addFd 线程安全问题
    // 原代码 addFd 在主线程直接写 conns/wheel/epoll_ctl，与 SubReactor 线程竞争
    // 导致 unordered_map rehash 时迭代器失效 → free(): invalid pointer
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