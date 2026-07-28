#include "ServerRuntime.h"
#include "server/Reactor/ReactorGroup.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <cstring>
#include <thread>
#include <signal.h>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <netinet/tcp.h>

#include "log/logger/logger.h"
#include "server/http/RequestContext/RequestContext.h"

ServerRuntime::ServerRuntime()
    : router_(std::make_shared<Router>()),
      codec_(*router_),
      executor_(std::clamp(
          std::thread::hardware_concurrency() == 0 ? 4u : std::thread::hardware_concurrency(), 2u, 32u))
{
    reactorGroup_ = std::make_unique<ReactorGroup>(codec_, executor_);
}

ServerRuntime::~ServerRuntime()
{
    // 先停止并 join Reactor，确保不会再向 Executor 提交任务；成员析构随后先 drain
    // Executor，再销毁仍持有 Session/协程帧的 Reactor，避免 shutdown 期间悬空访问。
    if (reactorGroup_)
    {
        reactorGroup_->stop();
        reactorGroup_->join();
    }
    if (listenFd_ >= 0)
        close(listenFd_);
    if (epfd_ >= 0)
        close(epfd_);
}

// 由于本次架构实现的是一io接受+多reactor组，所以每个reactor组也需要有对应的监听
void ServerRuntime::setupListener()
{
    // 忽略管道破裂信号SIGPIPE
    // 场景：当服务端向已断开的客户端socket写入数据时，系统会触发SIGPIPE信号
    // 默认行为是直接杀死进程，这里设置SIG_IGN忽略该信号，程序不会崩溃，通过write返回-1感知断开
    signal(SIGPIPE, SIG_IGN);

    // 创建监听socket文件描述符 listenFd_
    // 参数1 AF_INET：使用IPv4协议族
    // 参数2 SOCK_STREAM：TCP流式套接字；SOCK_NONBLOCK：设置socket为非阻塞IO；SOCK_CLOEXEC：执行exec系列函数时自动关闭该fd，避免fd泄漏
    // 参数3 0：对应TCP协议
    listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    // socket创建失败校验，返回-1代表出错
    if (listenFd_ < 0)
    {
        // strerror(errno) 将全局错误码转换为可读错误字符串，抛出运行时异常终止初始化
        throw std::runtime_error(std::string("socket: ") + strerror(errno));
    }

    int opt = 1;
    // 设置socket端口复用属性
    // SOL_SOCKET：套接字通用选项层级
    // SO_REUSEADDR：允许端口处于TIME_WAIT状态时，程序重新绑定该端口，解决服务重启端口占用问题
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 定义IPv4地址结构体，{}零初始化所有成员
    struct sockaddr_in addr{};
    addr.sin_addr.s_addr = INADDR_ANY; // INADDR_ANY=0.0.0.0，监听本机所有网卡IP
    addr.sin_family = AF_INET;         // 地址协议族IPv4
    addr.sin_port = htons(port_);      // htons：主机字节序转网络大端字节序，port_为外部传入监听端口

    // 将socket与指定IP、端口进行绑定
    // reinterpret_cast<sockaddr *>：sockaddr_in是IPv4专用地址结构，强制转换为通用sockaddr结构体指针
    if (bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        throw std::runtime_error(std::string("bind: ") + strerror(errno));
    }

    // 开启端口监听，SOMAXCONN为系统定义最大待连接队列长度
    if (listen(listenFd_, SOMAXCONN) < 0)
    {
        throw std::runtime_error(std::string("listen: ") + strerror(errno));
    }

    // 创建epoll实例，EPOLL_CLOEXEC：exec执行时自动关闭epoll fd，防止fd泄漏
    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    // epoll实例创建失败校验
    if (epfd_ == -1)
    {
        throw std::runtime_error(std::string("epoll_create1: ") + strerror(errno));
    }

    // 定义epoll事件结构体，零初始化
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET; // EPOLLIN：监听可读事件（新连接到达）；EPOLLET：开启边缘触发模式
    ev.data.fd = listenFd_;        // 绑定需要监听的文件描述符：服务端监听fd

    // 将监听fd注册进epoll实例，添加监听事件
    // EPOLL_CTL_ADD：操作类型-新增fd监听
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listenFd_, &ev) == -1)
        throw std::runtime_error(std::string("epoll_ctl listener: ") + strerror(errno));

    // 服务初始化完成，打印启动日志
    std::cout << "server started..." << std::endl;
}

void ServerRuntime::createReactors()
{
    // 确定reactor数组大小
    size_t n = reactorCount_;
    if (n == 0)
    {
        n = std::thread::hardware_concurrency();
        if (n == 0)
            n = 4;
        n = std::clamp<size_t>(n, 1, 32);
    }
    // 启动
    reactorGroup_->start(n);
}

void ServerRuntime::acceptLoop()
{
    // 开始接收来自reactor发送或者触发的信息给到reactors
    epoll_event events[1024];
    while (true)
    {
        int n = epoll_wait(epfd_, events, 1024, -1);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(std::string("acceptor epoll_wait: ") + strerror(errno));
        }
        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;
            if (fd == listenFd_)
            {
                struct sockaddr_in cin{};
                socklen_t socklen = sizeof(cin);
                while (true)
                {
                    int newfd = accept4(
                        listenFd_,
                        reinterpret_cast<sockaddr *>(&cin),
                        &socklen,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);
                    
                    if (newfd == -1)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        if (errno == EINTR)
                            continue;
                        LOG_ERROR(std::string("accept error") + strerror(errno));
                        break;
                    }
                    int yes = 1;
                    setsockopt(newfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                    LOG_INFO(std::string("new connection fd=") + std::to_string(newfd));
                    reactorGroup_->dispatch(newfd);
                }
            }
        }
    }
}

void ServerRuntime::start()
{
    setupListener();
    createReactors();
    acceptLoop();
}