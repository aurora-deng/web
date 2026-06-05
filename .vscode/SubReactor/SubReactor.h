#ifndef SUBREACTOR_H
#define SUBREACTOR_H

#include <cstdint>
#include <string>
#include <iostream>
#include <stdlib.h>
#include <thread>
#include <unordered_map>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <unistd.h>
#include <strings.h>
#include <sys/socket.h>
#include "server/http/http.h"
#include "server/threadpoll/thread_pool.h"
#include "server/Route/Router.h"
#include "server/Buffer/Buffer.h"
#include <unordered_set>
#define MAX_EVENTS 1024
#define KB(x) ((x) * 1024UL)
#define MB(x) ((x) * 1024UL * 1024UL)
#define MAX_PENDING_BYTES MB(4) // 最大数据长
#define MAX_PIPELINE 1024      // 最大任务提交数
extern ThreadPool pool;         // 公用main函数全局的线程池
// 优化，使用多reactor，每个reactor拥有独自的epoll，
// 并且每个reactor独自占领独自资源实现类似单独进程的作用，拥有自己的资源，从而实现无锁


struct ConnState
{
    bool closed = false;
    // bool wantRead=true;
    bool pauseByPipeline = false;
    bool pauseByMemory = false;
    bool wantWrite = false;
    bool readPaused = false;
};



struct PendingRequest
{
    uint64_t seq;
    HttpRequest data;
};

struct FileBody
{
    int fd=-1;
    off_t size=0;
    off_t offset=0;
    off_t remain=0;
};


struct pendingResponse
{
    // std::string data;
    // size_t offset = 0; // 优化：通过使用偏移量来判断数据是否发完，提效
    // 进一步优化，实现零拷贝,但是在发送之前需要进行常规和chunked判断
    bool chunked=false;
    bool useSendfile=false;

    size_t headerOffset=0;
    size_t chunkedIndex=0;
    size_t bodyOffset=0;
    size_t chunkOffset=0;
    size_t EndchunkOffset=0;

    std::string header;
    ResponseBodyPtr body;
    FileBody filebody;
    // std::deque<ChunkBolck> chunks;
    
    StreamQueuePtr stream;

    std::string endChunks="0\r\n\r\n";
    
};



struct Connection
{
    bool keepAlive;
    int fd;
    uint64_t id;
    size_t pendingBytes = 0;  // 统计目前fd中已经储存的请求数据的总字节量，用于控制合适的时候拒绝read数据保持待机状态
    size_t inflightTasks = 0; // 表示限制任务处理的提交数量太多
                              //  用于高并发下回复一致性
    uint64_t nextRequestSeq = 0;  // 生成请求编号
    uint64_t nextResponseSeq = 0; // 生成响应编号
    Buffer readBuffer;
    ConnState state;
    std::queue<PendingRequest> pendingRequests;          // 消息队列用于分离请求，缓解readBuffer即做缓冲队列又做请求处理队列
    std::map<uint64_t, pendingResponse> pendingResponses; // 用于当做回复消息的队列，同时，通过map精准对应请求seq_id
    // 增加时间轮,创立时间戳
    std::chrono::steady_clock::time_point lastActive;
    // 记录运行时间
    uint64_t activeTick=0;
};

class TimerWheel{
    public:
        void add(int fd);
        void refresh(int fd);

        void tick();

    private:
    static constexpr int SLOT=60;
    uint64_t currentTick=0;
    int cur=0;

    std::vector<std::unordered_set<int>>wheel;
};

struct TaskResult
{
    bool keepAlive=false;

    bool chunked=false;

    int fd=-1;
    uint64_t id=0;  // fd链接id
    uint64_t seq=0; // 用于当做响应请求时候的id

    // 处理静态文件
    bool useSendfile=false;
    int fileFd=-1;
    size_t sendBegin=0;
    size_t sendEnd=0;
    off_t fileSize=0;

    std::string header;
    ResponseBodyPtr body;

    // std::deque<ChunkBolck> chunks;
    StreamQueuePtr stream;

    ConnState state;
};

// 用于唤醒stream使用的结构体
struct StreamNotify
{
    int fd;
    uint64_t connId;
};

class SubReactor
{
public:
    int epfd;
    std::unordered_map<int, Connection> conns; // 接受发送类
    std::thread th;
    int event_fd;

    uint64_t currentTick=0;         //记录全局时间
    // 将worker线程中的response加入到队列里面去
    std::mutex queue_mtx;
    std::queue<TaskResult> push_to_SubReactor_queue;
    Router &router;
    std::queue<StreamNotify> StreamNotifyQueue;     //用于处理流式id和fd

    SubReactor(Router &router):router(router)
    {
        // 创建属于自己的epoll
        epfd = epoll_create(1);
        // 用于实现多线程之间通信 + 唤醒 epoll
        event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = event_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &ev);

    }

    void run();
    void pushResult(const TaskResult &res);
    void notifyStream(int fd,uint64_t id);
    // 统一套接字关闭
    void fd_close(int fd);

    // 设置fd为非堵塞，对于新添加的fd都要使用
    void fd_unblock(int fd);

    // 重新唤醒、
    void rearm(int fd, uint32_t events);

    // 用于优化集成rearm,结构性优化，接纳允许同时write和read
    void updateEvent(int fd);
    // 超时检查
    void checkTimeout();
    // 写入函数
    void handleWrite(int fd);
    bool handleTaskResultOnce();
    bool handleStreamNotify();
    // 读取函数
    void handleRead(int fd);
    bool processRequest(int fd);
    void loop();
    void addFd(int fd);
};

#endif