// =============================================================================
// 文件名：ServerRuntime.cpp
// ------------------------------------------------------------
// 【职责比喻：工厂总装线 / 酒店总经理的"工作手册"】
// 实现服务器运行时——信号处理、监听套接字建立、acceptor 事件循环、
// SubReactor 创建与优雅退出。本文件是"总经理"的完整工作手册：
// 开店前布置前台（setupListener 创建监听 socket 和 epoll），招楼层经理（createReactors
// 启动 SubReactor 组），登记信号处理（Ctrl+C 触发优雅退出），自己坐前台 acceptLoop 等客人
// 来——每来一位客人 accept4 接进来，分给下一位楼层经理（dispatch）。接到 Ctrl+C 信号后
// 先放掉前台端口（releaseListener），再通知各楼层经理收工（stop+join）。
//
// 【第四阶段重构在 cpp 中的体现】
// 构造函数初始化列表删除了 codec_(*router_) 一项；构造 ReactorGroup 时改传 *router_
// 而非 codec_。第四阶段前 Runtime 持有 HttpCodec codec_ 成员，并下传给所有 SubReactor
// 共享——那时只有 HTTP 一种协议没问题。引入 WebSocket 后 HTTP/WS 编解码互不兼容，
// 共享 codec 会串扰；故删除 codec_ 成员，codec 下沉到各 Session 子类内部，
// Runtime 只装配协议无关的 Router、两条 Executor 容量通道、WS/SSE 管理器，外加
// SessionFactory 实现 sessionFactory_（ProtocolSessionFactory）——后者经 ReactorGroup::start
// 注入每个 SubReactor，用于在 HTTP 首部发完后创建 WebSocketSession 或 SseSession。
//
// 关键技术点（初学者重点理解）：
// 1. acceptor 用 epoll + 非阻塞 listenFd：让 acceptLoop 能被 wakeFd_ 唤醒退出，
//    而不是死卡在 accept() 里；同时多核能并行 accept（虽然本实现单 acceptor）。
// 2. SIGPIPE 必须忽略：向已断开的 socket 写会触发 SIGPIPE，默认杀进程，要 SIG_IGN。
// 3. SO_REUSEADDR：服务重启时端口可能还在 TIME_WAIT 状态，不设这个会 bind 失败。
// 4. TCP_NODELAY：禁用 Nagle 算法，避免小响应（如 HTTP 头）被攒 40ms 才发，
//    对 HTTP 响应延迟极其关键。
// 5. maxConnections_ 软限流：accept 时检查总连接数，超额直接 close 新 fd，
//    避免无界接入撑爆 fd/内存。
// 6. 优雅退出顺序：stop() 置 running_=false + 写 wakeFd_ → acceptLoop 醒来退出 →
//    wsDelivery_ stop+join → releaseListener → reactorGroup_->stop+join（对象保留）→
//    HTTP/WS Executor drain Worker → 最后销毁 ReactorGroup。
// 7. 组件装配顺序：构造期 ①make_shared<Router> 建路由表；②把 4~32 个 Worker
//    近似均分给 HTTP 与 WS 两个 Executor；③WS 工厂拿 wsExecutor_，ReactorGroup 拿
//    httpExecutor_；④start() 时 setupListener + createReactors，再绑定 manager。
//    ④start() 时 setupListener + createReactors，再 wsManager_.bind(reactorGroup_)。
// =============================================================================
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
#include <atomic>

#include "log/logger/logger.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/session/ProtocolSessionFactory.h"
#include "server/tls/TlsContext.h"

// 匿名命名空间：内部链接，只在本文件可见
namespace
{
    // 全局 ServerRuntime 指针，供信号处理函数访问
    // 用 atomic 保证信号处理函数与主线程的访问是线程安全的
    std::atomic<ServerRuntime *> g_runtime{nullptr};

    // 总 Worker 数限制为 4~32，再均分成两个至少两人的独立容量舱。
    size_t runtimeWorkerCount()
    {
        static const size_t count = std::clamp<size_t>(
            std::thread::hardware_concurrency() == 0
                ? 4u
                : std::thread::hardware_concurrency(),
            4u,
            32u);
        return count;
    }

    /**
     * @brief SIGINT/SIGTERM 信号处理函数
     *
     * 【通俗解释】
     * 收到 Ctrl+C 或 kill 信号时被调用。信号处理函数有严格限制——只能做
     * async-signal-safe 的操作（不能调 malloc/printf/锁等），所以这里只做两件事：
     * ①取 g_runtime 指针；②调 requestStop()（内部是 atomic store + write eventfd，
     * 都是 async-signal-safe）。剩下的退出流程交给主循环自己处理。
     */
    void handleStopSignal(int)
    {
        // 仅做 async-signal-safe 操作：置位并由 eventfd 唤醒 accept 循环。
        if (auto *runtime = g_runtime.load(std::memory_order_acquire))
            runtime->requestStop();
    }
} // namespace

/**
 * @brief 构造函数：创建 Router 和两条 Executor 容量通道，初始化 ReactorGroup
 *
 * 【通俗解释】
 * 总经理上任：①建一本菜谱（Router）；②按 CPU 核数招 4~32 名后厨厨师，
 * 近似均分到 HTTP 与 WS 两间厨房，避免一类慢任务吃光另一类容量；③建 ReactorGroup。
 *
 * @note 第四阶段重构前初始化列表里有 codec_(*router_) 一项（用 router 构造共享 HttpCodec），
 *       构造 ReactorGroup 时传的是 codec_。第四阶段删除了 codec_ 成员——codec 下沉到各
 *       Session 子类内部，Runtime 不再创建 codec 实例；构造 ReactorGroup 改传 *router_
 *       （路由器协议无关，HTTP/WS 共用）。这样同一套 Runtime 可同时承载 HTTP 与 WS 协议。
 */
ServerRuntime::ServerRuntime()
    : router_(std::make_shared<Router>()),
      wsDelivery_([this](UserId uid, std::string text)
                  {
          return wsManager_.sendTextTracked(uid, text);
      }),
      httpExecutor_((runtimeWorkerCount() + 1) / 2),
      wsExecutor_(runtimeWorkerCount() / 2)
{
    // ---- SessionFactory 依赖注入 ----
    // ①创建 ProtocolSessionFactory（SessionFactory 抽象的具体实现），内部封装
    //   WebSocket 与 SSE 的会话依赖——这样 SubReactor/ReactorGroup
    //   后续只看 SessionFactory* 抽象指针，不直接依赖 WebSocket/SSE 具体类型。
    // ②把 *sessionFactory_ 引用传给 ReactorGroup 构造，ReactorGroup::start 时会调
    //   每个 SubReactor::setSessionFactory(&sessionFactory_) 完成注入。后续 SubReactor
    //   在 HTTP 首部发完后通过同一接口创建 WebSocketSession 或 SseSession。
    sessionFactory_ = std::make_unique<ProtocolSessionFactory>(
        wsManager_, wsDispatcher_, wsExecutor_, sseManager_);
    reactorGroup_ = std::make_unique<ReactorGroup>(
        *router_, httpExecutor_, *sessionFactory_);
}

/**
 * @brief 析构函数：请求停止 + 关闭所有 fd + 清全局指针
 *
 * 【通俗解释】
 * 总经理离职：①requestStop 通知 acceptLoop 退出；②关 listenFd/wakeFd_/epfd_;
 * ③清 g_runtime 全局指针（防止信号处理函数访问已析构对象）。
 * Reactor 对象必须活到 Executor 排空以后，因此这里显式执行完整关闭顺序。
 */
ServerRuntime::~ServerRuntime()
{
    shutdownComponents();
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
    // 清全局指针，防止信号处理函数访问已析构对象
    if (g_runtime.load(std::memory_order_acquire) == this)
        g_runtime.store(nullptr, std::memory_order_release);
}

void ServerRuntime::shutdownComponents() noexcept
{
    // ①先停所有任务生产者：acceptor、可靠投递重试器与 Reactor 事件线程。
    wsDelivery_.stop();
    requestStop();
    releaseListener();
    if (reactorGroup_)
    {
        reactorGroup_->stop();
        reactorGroup_->join();
    }

    // ②Reactor 线程已不再提交业务，但 Reactor 对象/eventfd 仍活着。排空 Worker 时，
    //   已在途任务仍可安全调用 notifyExecuteComplete/postOutbound。
    httpExecutor_.shutdown();
    wsExecutor_.shutdown();

    // ③所有 Worker 都退出后才销毁 Session、协程帧和 Reactor 回调目标。
    if (reactorGroup_)
    {
        wsManager_.bind(nullptr);
        sseManager_.bind(nullptr);
        reactorGroup_.reset();
    }
}

// 由于本次架构实现的是一io接受+多reactor组，所以每个reactor组也需要有对应的监听
/**
 * @brief 建立监听套接字：socket→setsockopt→bind→listen→epoll 注册
 *
 * 【通俗解释】
 * 总经理布置前台：①忽略 SIGPIPE（向断开 socket 写不再杀进程）；
 * ②建一个非阻塞监听 socket；③SO_REUSEADDR 允许端口复用（重启不卡 TIME_WAIT）；
 * ④bind 到 0.0.0.0:8080；⑤listen 开始接客；⑥建 epoll 并注册 listenFd + wakeFd_。
 * 失败任何一步都抛异常终止启动。
 */
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

    // wakeFd_ 用于优雅退出：stop() 写 wakeFd_ 唤醒阻塞在 epoll_wait 的 acceptLoop
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

    if (tlsContext_) {
        if (tlsPort_ <= 0 || tlsPort_ > 65535 || tlsPort_ == port_)
            throw std::runtime_error("TLS port must be distinct and in 1..65535");
        tlsListenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (tlsListenFd_ < 0)
            throw std::runtime_error("TLS listener socket failed");
        setsockopt(tlsListenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in tlsAddr{};
        tlsAddr.sin_family = AF_INET;
        tlsAddr.sin_addr.s_addr = INADDR_ANY;
        tlsAddr.sin_port = htons(tlsPort_);
        if (bind(tlsListenFd_, reinterpret_cast<sockaddr *>(&tlsAddr), sizeof(tlsAddr)) < 0 ||
            listen(tlsListenFd_, SOMAXCONN) < 0)
            throw std::runtime_error(std::string("TLS bind/listen: ") + strerror(errno));
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = tlsListenFd_;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, tlsListenFd_, &ev) < 0)
            throw std::runtime_error("epoll_ctl TLS listener failed");
    }

    // wakeFd_ 用水平触发，避免漏唤醒
    ev.events = EPOLLIN;
    ev.data.fd = wakeFd_;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, wakeFd_, &ev) == -1)
        throw std::runtime_error(std::string("epoll_ctl wake: ") + strerror(errno));

    // 服务初始化完成，打印启动日志
    std::cout << "server started..." << std::endl;
}

/**
 * @brief 请求服务器停止（线程安全，可由信号处理函数调用）
 *
 * 【通俗解释】
 * 给前台发"打烊"信号：①running_=false（主循环看见就退出）；
 * ②写 wakeFd_ 把可能阻塞在 epoll_wait 的 acceptLoop 立即唤醒。
 * 这两步都是 async-signal-safe，可在信号处理函数里直接调。
 */
void ServerRuntime::requestStop()
{
    running_.store(false, std::memory_order_release);
    if (wakeFd_ >= 0)
    {
        uint64_t one = 1;
        (void)write(wakeFd_, &one, sizeof(one));
    }
}

/**
 * @brief 立即释放监听套接字（不阻塞）
 *
 * 【通俗解释】
 * 把 8080 端口还给系统，不必等 Reactor 全部 join 完。这样重启服务时能立即绑定端口，
 * 避免 TIME_WAIT 等待。从 epoll 注销 listenFd 后 close 它。
 */
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
    if (tlsListenFd_ >= 0) {
        if (epfd_ >= 0)
            (void)epoll_ctl(epfd_, EPOLL_CTL_DEL, tlsListenFd_, nullptr);
        close(tlsListenFd_);
        tlsListenFd_ = -1;
    }
}

/**
 * @brief 创建并启动 SubReactor 组
 *
 * 【通俗解释】
 * 决定招多少楼层经理：①若用户没指定（reactorCount_==0），按 CPU 核数取值，
 * 限幅在 1~32 之间；②调 reactorGroup_->start(n) 一次性招 n 个并启动各自的事件循环线程。
 */
void ServerRuntime::createReactors()
{
    // 确定reactor数组大小
    size_t n = reactorCount_;
    if (n == 0)
    {
        // 默认按 CPU 核数（hardware_concurrency），取不到则用 4
        n = std::thread::hardware_concurrency();
        if (n == 0)
            n = 4;
        // 限幅 1~32：太少不够用，太多线程切换开销大
        n = std::clamp<size_t>(n, 1, 32);
    }
    // 启动
    reactorGroup_->setTlsContext(tlsContext_);
    reactorGroup_->start(n);
    // 绑定发生在 Runtime：ReactorGroup 保持协议无关
    wsManager_.bind(reactorGroup_.get());
    sseManager_.bind(reactorGroup_.get());
}

/**
 * @brief acceptor 事件循环：accept 新连接并 dispatch 到 SubReactor
 *
 * 【通俗解释】
 * 总经理坐前台值班：
 *   while (running) {
 *     ①epoll_wait 永久阻塞等事件（-1 超时）；
 *     ②wakeFd_ 触发 → 排空 wakeFd_，回到循环顶检查 running 退出；
 *     ③listenFd_ 触发 → while accept4 接所有积压连接：
 *        - 设 TCP_NODELAY（禁 Nagle，避免 40ms 延迟）；
 *        - 检查全局连接数限流，超额直接 close；
 *        - reactorGroup_->dispatch(newfd) 轮询分给下一个 SubReactor。
 *   }
 * 关键点：accept4 用 SOCK_NONBLOCK 直接返回非阻塞 fd，省一次 fcntl；
 * EAGAIN 表示暂无更多连接，break 内层 while 继续 epoll_wait。
 */
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
            if (fd == listenFd_ || fd == tlsListenFd_)
            {
                struct sockaddr_in cin{};
                socklen_t socklen = sizeof(cin);
                // 边缘触发模式下必须循环 accept 直到 EAGAIN，否则会漏接
                while (running_.load(std::memory_order_acquire))
                {
                    int newfd = accept4(
                        fd,
                        reinterpret_cast<sockaddr *>(&cin),
                        &socklen,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);

                    if (newfd == -1)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break; // 暂无更多连接，回到 epoll_wait
                        if (errno == EINTR)
                            continue;
                        LOG_ERROR(std::string("accept error") + strerror(errno));
                        break;
                    }

                    // 关闭 Nagle，避免小响应头/体分写触发 Delayed ACK ~40ms 地板。
                    // 这是 HTTP 响应延迟优化的关键——Nagle 会让小包攒着等 ACK，导致 40ms 延迟
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
                    // dispatch 内部轮询分给下一个 SubReactor，SubReactor::addFd 投递到自己的待处理队列
                    reactorGroup_->dispatch(newfd, fd == tlsListenFd_);
                } // end while accept4
            } // end listenFd
        } // end for events
    } // end while running
}

/**
 * @brief 启动服务器：建监听→建 Reactor→装信号→跑 acceptLoop→优雅退出
 *
 * 【通俗解释】
 * 总经理开店全流程：
 *   ①setupListener 布置前台（建 listenFd/epfd/wakeFd）；
 *   ②createReactors 招楼层经理（启动 SubReactor 组）；
 *   ③启动 wsDelivery_ 自动重试调度员；
 *   ④登记 SIGINT/SIGTERM 信号处理（Ctrl+C 触发 handleStopSignal→requestStop）；
 *   ⑤acceptLoop 阻塞等客人（直到 running_=false 退出）；
 *   ⑥停任务生产者，stop+join Reactor 线程；⑦保留 Reactor 对象并 drain Executor；
 *   ⑧销毁 ReactorGroup，清 g_runtime，打印 "server stopped"。
 * 整个 start 是阻塞的——只有完全退出后才返回，调用方一般是 main 函数。
 */
void ServerRuntime::start()
{
    if (tlsCertificate_.empty() != tlsPrivateKey_.empty())
        throw std::runtime_error("TLS requires both certificate and private key");
    if (!tlsCertificate_.empty())
        tlsContext_ = TlsContext::create(tlsCertificate_, tlsPrivateKey_);
    setupListener();
    auto shutdown = [this]
    {
        shutdownComponents();
        g_runtime.store(nullptr, std::memory_order_release);
    };

    try
    {
        createReactors();
        wsDelivery_.start();
        // 登记 g_runtime 让信号处理函数能找到本对象
        g_runtime.store(this, std::memory_order_release);
        // 注册信号处理：Ctrl+C 或 kill 触发 handleStopSignal→requestStop
        std::signal(SIGINT, handleStopSignal);
        std::signal(SIGTERM, handleStopSignal);
        // 阻塞在 acceptor loop，直到 running_=false
        acceptLoop();
    }
    catch (...)
    {
        shutdown();
        throw;
    }
    shutdown();
    std::cout << "server stopped." << std::endl;
}
