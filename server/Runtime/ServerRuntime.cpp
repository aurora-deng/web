#include "ServerRuntime.h"
#include "server/Reactor/ReactorGroup.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <cstring>
#include <thread>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <netinet/tcp.h>
#include <atomic>

#include "log/logger/logger.h"
#include "server/http/RequestContext/RequestContext.h"

namespace
{
    std::atomic<ServerRuntime *> g_runtime{nullptr};

    void handleStopSignal(int)
    {
        // 仅做 async-signal-safe 操作：置位并由 eventfd 唤醒 accept 循环。
        if (auto *runtime = g_runtime.load(std::memory_order_acquire))
            runtime->requestStop();
    }
} // namespace

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
    requestStop();
    // 先停止并 join Reactor，确保不会再向 Executor 提交任务；成员析构随后先 drain
    // Executor，再销毁仍持有 Session/协程帧的 Reactor，避免 shutdown 期间悬空访问。
    if (reactorGroup_)
    {
        close(listenFd_);
        listenFd_ = -1;
    }
    if (wakeFd_ >= 0)
    {
        close(wakeFd_);
        wakeFd_ = -1;
    }
    if (epfd_ >= 0)
    {
        close(epfd_);
        epfd_ = -1;
    }
    if (g_runtime.load(std::memory_order_acquire) == this)
        g_runtime.store(nullptr, std::memory_order_release);
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

    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0)
        throw std::runtime_error(std::string("eventfd wake: ") + strerror(errno));

    // 定义epoll事件结构体，零初始化
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET; // EPOLLIN：监听可读事件（新连接到达）；EPOLLET：开启边缘触发模式
    ev.data.fd = listenFd_;        // 绑定需要监听的文件描述符：服务端监听fd

    // 将监听fd注册进epoll实例，添加监听事件
    // EPOLL_CTL_ADD：操作类型-新增fd监听
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listenFd_, &ev) == -1)
        throw std::runtime_error(std::string("epoll_ctl listener: ") + strerror(errno));

    ev.events = EPOLLIN;
    ev.data.fd = wakeFd_;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, wakeFd_, &ev) == -1)
        throw std::runtime_error(std::string("epoll_ctl wake: ") + strerror(errno));

    // 服务初始化完成，打印启动日志
    std::cout << "server started..." << std::endl;
}

void ServerRuntime::requestStop()
{
    running_.store(false, std::memory_order_release);
    if (wakeFd_ >= 0)
    {
        uint64_t one = 1;
        (void)write(wakeFd_, &one, sizeof(one));
    }
}

void ServerRuntime::releaseListener()
{
    // 尽早关闭 listen，把 8080 还给系统；不必等 Reactor join 结束。
    if (listenFd_ >= 0)
    {
        if (epfd_ >= 0)
            (void)epoll_ctl(epfd_, EPOLL_CTL_DEL, listenFd_, nullptr);
        close(listenFd_);
        listenFd_ = -1;
    }
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
    // running_ 原子标记，控制服务启停，acquire保证内存可见,使得安全退出
    while (running_.load(std::memory_order_acquire))
    {
        // 正常监听
        int n = epoll_wait(epfd_, events, 1024, -1);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(std::string("acceptor epoll_wait: ") + strerror(errno));
        }
        // 收到事件后再次检查停止标记，防止shutdown竞争
        if (!running_.load(std::memory_order_acquire))
            break;

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;
            // ========== 1. 唤醒管道 wakeFd_ ==========
            if (fd == wakeFd_)
            {
                uint64_t cnt = 0;
                //  排空缓冲区，避免多次唤醒堆积
                // 服务执行shutdown()的时候，
                // 向 wakeFd 写入数据，唤醒阻塞在epoll_wait的 accept 线程，
                // 让循环感知running_=false正常退出。，用于退出处理，防止强制退出导致重启失败
                while (read(wakeFd_, &cnt, sizeof(cnt)) > 0)
                    ;
                continue;
            }
            // 正常监听套字节
            if (fd == listenFd_)
            {
                struct sockaddr_in cin{};
                socklen_t socklen = sizeof(cin);
                while (running_.load(std::memory_order_acquire))
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

                    // 关闭 Nagle，避免小响应头/体分写触发 Delayed ACK ~40ms 地板。
                    int yes = 1;
                    if (setsockopt(newfd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1)
                    {
                        LOG_ERROR(std::string("TCP_NODELAY: ") + strerror(errno));
                        close(newfd);
                        continue;
                    }
                    // 全局最大连接数限流
                    if (maxConnections_ > 0 &&
                        reactorGroup_->activeConnections() >= maxConnections_)
                    {
                        // accept 侧软限流：超额连接立即关闭，避免无界占满 fd/内存。
                        close(newfd);
                        continue;
                    }

                    LOG_INFO(std::string("new connection fd=") + std::to_string(newfd));
                    // 分发fd到Reactor线程池
                    reactorGroup_->dispatch(newfd);
                } // end while accept4
            } // end listenFd
        } // end for events
    } // end while running
}

void ServerRuntime::start()
{
    setupListener();
    createReactors();
    g_runtime.store(this, std::memory_order_release);
    std::signal(SIGINT, handleStopSignal);
    std::signal(SIGTERM, handleStopSignal);

    acceptLoop();

    // Ctrl+C 路径：立刻释放监听端口，再回收 Reactor，避免 join 期间端口仍被占用。
    releaseListener();

    // 停止接受后回收 Reactor，再返回；Executor 随成员析构 drain。
    if (reactorGroup_)
    {
        reactorGroup_->stop();
        reactorGroup_->join();
    }
    g_runtime.store(nullptr, std::memory_order_release);
    std::cout << "server stopped." << std::endl;
}