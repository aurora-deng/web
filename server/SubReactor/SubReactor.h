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
#include <unistd.h>
#include <strings.h>
#include <sys/socket.h>
#include "server/http/http.h"
#include"server/threadpoll/thread_pool.h"
#define MAX_EVENTS 1024
#define KB(x) ((x) * 1024UL)
#define MB(x) ((x) * 1024UL * 1024UL)
#define MAX_PENDING_BYTES MB(4)     //最大数据长
#define MAX_PIPELINE   10240             //最大任务提交数
extern ThreadPool pool;     //公用main函数全局的线程池
// 优化，使用多reactor，每个reactor拥有独自的epoll，
// 并且每个reactor独自占领独自资源实现类似单独进程的作用，拥有自己的资源，从而实现无锁
struct ConnState
{
    bool closed=false;
    // bool wantRead=true;
    bool pauseByPipeline=false;
    bool pauseByMemory=false;
    bool wantWrite=false;
    bool readPaused=false;
};

struct PendingRequest
{
    uint64_t seq;
    std::string data;
};

struct pedingResponse
{
    std::string data;
    size_t offset=0;     //优化：通过使用偏移量来判断数据是否发完，提效
};
struct Connection
{
    bool keepAlive;
    int fd;
    uint64_t id;
    size_t pendingBytes=0;                     //统计目前fd中已经储存的请求数据的总字节量，用于控制合适的时候拒绝read数据保持待机状态
    size_t inflightTasks=0;                    //表示限制任务处理的提交数量太多
     // 用于高并发下回复一致性
    uint64_t nextRequestSeq=0;                //生成请求编号
    uint64_t nextResponseSeq=0;               //生成响应编号
    std::string readBuffer;
    ConnState state;
    std::queue<PendingRequest> pendingRequests;            //消息队列用于分离请求，缓解readBuffer即做缓冲队列又做请求处理队列
    std::map<uint64_t,pedingResponse> pendingResponses;         //用于当做回复消息的队列，同时，通过map精准对应请求seq_id
};


struct TaskResult
{
    int fd;
    uint64_t id;            //fd链接id
    uint64_t seq;           //用于当做响应请求时候的id
    std::string response;
    bool keepAlive;
    ConnState state;
};

class SubReactor
{
public:
    int epfd;
    std::unordered_map<int, Connection> conns; // 接受发送类
    std::thread th;
    int event_fd;
    // 将worker线程中的response加入到队列里面去
    std::mutex queue_mtx;
    std::queue<TaskResult> push_to_SubReactor_queue; 

    SubReactor()
    {
        // 创建属于自己的epoll
        epfd = epoll_create(1);
        // 用于实现多线程之间通信 + 唤醒 epoll
        event_fd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
        epoll_event ev{};
        ev.events=EPOLLIN;
        ev.data.fd=event_fd;
        epoll_ctl(epfd,EPOLL_CTL_ADD,event_fd,&ev);
    }

    void run();
    void pushResult(const TaskResult& res);
    // 统一套接字关闭
    void fd_close(int fd);
    
    // 设置fd为非堵塞，对于新添加的fd都要使用
    void fd_unblock(int fd);
    
    // 重新唤醒、
    void rearm(int fd, uint32_t events);

    // 用于优化集成rearm,结构性优化，接纳允许同时write和read
    void updateEvent(int fd);
   
    // 用于判断消息是否发送完
    bool is_complete(const std::string &buf);
    // 写入函数
    void handleWrite(int fd);
    bool handleTaskResultOnce();
    std::string extract_request(std::string &readBuffer);
    // 读取函数
    void handleRead(int fd);
    bool processRequest(int fd);
    void loop();
    void addFd(int fd);
};

#endif