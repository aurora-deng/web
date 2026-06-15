#include <sys/socket.h> // socket()、bind()、listen()、accept()、connect()、setsockopt() 等核心函数
#include <netinet/in.h> // sockaddr_in 结构体、htons()、htonl()、INADDR_ANY 等地址相关
#include <arpa/inet.h>  // inet_addr()、inet_ntoa()、inet_pton()、inet_ntop() IP地址转换
#include <fcntl.h>      // fcntl() 设置非阻塞、文件状态标志
#include <errno.h>      // errno 错误码、EAGAIN、EWOULDBLOCK 等错误定义
#include <unistd.h>     // close()、read()、write()、fork() 等文件/
#include <sys/select.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <string>
#include <iostream>
#include <strings.h>
#include <cstring>
#include <map>
#include <atomic> //自动累计函数库
#include <algorithm>
#include <stdlib.h>
#include<signal.h>
#include "server/threadpoll/thread_pool.h"
#include "server/http/http.h"
#include "server/Route/Router.h"
#include "server/Buffer/Buffer.h"
#include "server/SubReactor/SubReactor.h"
#include "log/logger/logger.h"

#define MAX_EVENTS 1024
using Middleware = std::function<bool(Context &)>;

// 添加状态机用于后续链接有利于正常处理当前的状态,替代之前的单一wantWrite
// enum ConnState
// {
//     READING,
//     WRITING,
//     CLOSED
// };

// struct Connection
// {
//     int fd;
//     uint64_t id;
//     std::string readBuffer;
//     std::string writeBuffer;
//     bool keepAlive;
//     ConnState state;
// };

// struct TaskResult
// {
//     int fd;
//     uint64_t id;
//     std::string response;
//     bool keepAlive;
//     ConnState state;
// };
int epfd = epoll_create(1);
// int event_fd = eventfd(0, EFD_NONBLOCK); // 作用：一个线程间唤醒epoll的fd
// 性能修复处：线程池大小从 4 改为 CPU 核心数
// 原代码 ThreadPool pool(4) 只有 4 个工作线程，5000 并发时请求排队导致 75% 延迟飙到 2289ms
// 工作线程负责执行 router.handle()（业务逻辑），是请求处理的主要瓶颈
ThreadPool pool(std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 4);




// 🚨 必须满足 3 个条件才算正确
// ✔ 1. id 全局唯一（atomic）
// ✔ 2. 任务带 id
// ✔ 3. 使用前校验 id

// 设置fd为非堵塞，对于新添加的fd都要使用
void fd_unblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 使得发到跳过time_wait可以直接复用,只要对listen使用
void fd_jump_time_wait(int fd)
{
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

int main(int argc, const char *argv[])
{
    // 崩溃修复处：忽略 SIGPIPE 信号
    // 当客户端关闭连接后，服务器再往该连接 write 会触发 SIGPIPE 信号
    // 默认行为是终止进程，这就是压测后服务器自动退出的根因
    // wrk 压测结束后客户端关闭连接，服务器还在发送响应 → write 触发 SIGPIPE → 进程被杀
    signal(SIGPIPE, SIG_IGN);
    int sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0)
    {
        LOG_ERROR(std::string("socket") + strerror(errno));
        return -1;
    }
    struct sockaddr_in addr;
    addr.sin_addr.s_addr = INADDR_ANY; // ip地址
    addr.sin_family = AF_INET;         // 通信域
    addr.sin_port = htons(8080);       // 端口号

    // 需要再bind前设置fd跳过time_wait
    fd_jump_time_wait(sockfd_);

    // bind(sockfd_,(sockaddr*)&addr,sizeof(addr));        //服务器套字节绑定ip地址
    if (bind(sockfd_, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_ERROR(std::string("bind error") + strerror(errno));
        return -1;
    }

    if (listen(sockfd_, 128) < 0)
    {
        LOG_ERROR(std::string("listen error") + strerror(errno));
        return -1;
    }

    // 使得套接字非堵塞
    fd_unblock(sockfd_);
    // 创建epoll

    if (epfd == -1)
    {
        LOG_ERROR(std::string("epoll_create error") + strerror(errno));
        return -1;
    }
    epoll_event ev{}, events[MAX_EVENTS];
    ev.events = EPOLLIN | EPOLLET; // 设置et模式
    ev.data.fd = sockfd_;          // 添加监听套接字

    // 绑定套接字到epfd上同时设置为添加状态,绑定监听套接字
    epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd_, &ev);

    // // 将eventfd添加到epoll中去
    // epoll_event ev1{};
    // ev1.events = EPOLLIN;
    // ev1.data.fd = event_fd;
    // epoll_ctl(epfd, EPOLL_CTL_ADD, event_fd, &ev1);

    std::cout << "server started..." << std::endl;

    struct sockaddr_in cin;
    socklen_t socklen = sizeof(cin);
    // 优化：使用共享指针来管理好router，使用唯一指针俩管理好sub,防止串台，同事遵循rall防止内存泄漏
    auto router = std::make_shared<Router>();  // ✅ 真正创建对象
    std::vector<std::unique_ptr<SubReactor>> subs;
    router->use([](Context &ctx) -> bool
                {
        std::cout<<ctx.req.method<<" "<<ctx.req.path<<std::endl;
        return true; });

    router->use([](Context &ctx) -> bool
                {
        if(ctx.req.path=="/admin")
        {
            auto it=ctx.req.headers.find("token");

            if(it==ctx.req.headers.end())
            {
                ctx.resp.status=401;
                ctx.resp.text("Unauthorized");

                return false;
            }
        }
        return true; });
    router->GET("/",
                [](Context &ctx)
                {
                    ctx.resp.html("<h1>hello</h1>");
                });

    router->GET("/user/:id",
                [](Context &ctx)
                {
                    ctx.resp.text(ctx.params.at("id"));
                });

    router->GET("/stream1",
                [](Context &ctx)
                {
                    ctx.resp.writeChunk("hello");
                    ctx.resp.writeChunk(" world");
                });

    router->GET("/stream2",
                [](Context &ctx)
                {
                    ctx.resp.beginChunked();

                    for (int i = 0; i < 10; i++)
                    {
                        ctx.resp.writeChunk(
                            "hello\n");

                        sleep(1);
                    }

                    ctx.resp.endChunked();
                });
    router->GET("/logo", [](Context &ctx)
                {
                    std::string path = "./static/logo.png";
                    auto it = ctx.req.headers.find("range");

                    ctx.resp.sendfile(path.c_str(),ctx.req.range); });
    // 返回你的计算机 物理上能并行执行的线程数量（逻辑核心数）
    int N = std::thread::hardware_concurrency();
    if (N == 0)
        N = 4; // fallback
    for (int i = 0; i < N; i++)
    {
        subs.push_back(std::make_unique<SubReactor>(*router));
        subs.back()->run();
    }
    int idx = 0;
    while (true)
    {

        // 持续监听epfd并且将事件放到events，接受任务
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        // 处理io
        for (int i = 0; i < n; i++)
        {
            // 取出监听事件
            int fd = events[i].data.fd; // 等价于// int client = accept(sockfd_, (struct sockaddr *)&cin, &socklen);
            // 当发现取出的监听sockfd_这个获取套接字时，说明有新的连接来了--------------处理新的监听
            if (fd == sockfd_)
            {
                // 由于使用的是et模式，因此必须要使用while保证每次都读完所有数据
                while (true)
                {
                    int newfd_ = accept(sockfd_, (struct sockaddr *)&cin, &socklen);
                    if (newfd_ == -1)
                    {

                        if (errno == EAGAIN)
                            break; // 说明没有多余链接了
                        else
                        {
                            LOG_ERROR(std::string("accept error") + strerror(errno));
                            break;
                        }
                    }
                    printf("[%s:%d]:已连接成功，newfd=%d!!!!\n", inet_ntoa(cin.sin_addr), ntohs(cin.sin_port), newfd_);
                    // 分配任务
                    subs[idx]->addFd(newfd_);
                    // subs[idx]->addPendingFd(newfd_);
                    idx = (idx + 1) % subs.size();
                }
            }
        }
    }
    return 0;
}
