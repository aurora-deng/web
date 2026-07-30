
#include "SubReactor.h"
#include <cctype>
#include <chrono>
std::atomic<uint64_t> global_conn_id{0}; // 自增

SubReactor::~SubReactor()
{
    // 先停止后散出
    stop();
    join();

    // 线程退出后不再有 epoll/连接表并发访问；直接关闭传输资源。
    // 协程帧随后由 CoroutineScheduler 析构统一销毁。
    for (auto &[fd, _] : conns)
        close(fd);
    conns.clear();
    if (event_fd >= 0)
        close(event_fd);
    if (epfd >= 0)
        close(epfd);
}

// 线程关闭策略
void SubReactor::join()
{
    if (th.joinable())
        th.join();
}

// 使用关闭触发唤醒策略，如果关闭就发送fd使得epoll知道，逻辑参考单个reactor里面的函数
void SubReactor::stop()
{
    // 判断是否早已关闭
    if (!running.exchange(false))
        return;
    uint64_t one = 1;
    // 发送关闭通知
    if (event_fd >= 0)
        (void)write(event_fd, &one, sizeof(one));
}

void SubReactor::updateEvent(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    uint32_t ev = 0;
    // 背压修复处：综合所有背压条件决定是否暂停读
    // 任一背压条件触发都应暂停读，防止数据继续涌入

    it->second->state.readPaused = it->second->state.pauseByMemory;
    if (!it->second->state.readPaused)
        ev |= EPOLLIN;

    if (it->second->state.wantWrite)
    {
        ev |= EPOLLOUT;
    }

    rearm(fd, ev);
}

void SubReactor::wakeReadCoroutine(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    auto &c = *it->second;
    auto &ctx = c.session->coroutine_context;
    if (ctx.state != AwaitType::READ)
        return; // 已在运行/已唤醒，不重复入队
    if (!ctx.handle || ctx.handle.done())
        return; // 无效 handle，跳过
    // 刷新状态
    ctx.state = AwaitType::NONE;
    ctx.waiting = false;
    auto h = ctx.handle;
    if (h)
    {
        scheduler.schedule(h);
    }
}

// 安全唤醒写协程，统一由 scheduler.schedule 去重。
void SubReactor::wakeWriteCoroutine(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    auto &ctx = it->second->session->coroutine_context;
    if (ctx.state != AwaitType::WRITE)
        return;
    if (!ctx.handle)
        return;
    ctx.state = AwaitType::NONE;
    ctx.waiting = false;
    auto h = ctx.handle;
    if (h)
    {
        scheduler.schedule(h);
    }
}

// 由于在io端的线程和在业务端的线程分离，所以协程唤醒需要有单独的函数来实现唤醒
// 唤醒执行完成的协程：Worker 线程完成 handler 后通过 processComplete 调用。
void SubReactor::wakeExecuteCoroutine(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    auto &ctx = it->second->session->coroutine_context;
    if (ctx.state != AwaitType::EXECUTE)
        return;
    if (!ctx.handle)
        return;
    ctx.state = AwaitType::NONE;
    ctx.waiting = false;
    scheduler.schedule(ctx.handle);
}

// Worker 线程调用（线程安全）：投递完成通知到 completeQueue，并通过 eventfd 唤醒 Reactor。
// 跨线程通信通知函数，通知work线程工作，实现队列缓存，保障跨线程通信安全，同时使用write提醒竹reactor
void SubReactor::notifyExecuteComplete(int fd, uint64_t connId)
{
    {
        std::lock_guard<std::mutex> lock(completeMtx);
        completeQueue.push({fd, connId});
    }
    if (!notified.exchange(true))
    {
        uint64_t one = 1;
        if (write(event_fd, &one, sizeof(one)) == -1)
        {
            if (errno != EAGAIN)
            {
                LOG_INFO(std::string("notifyExecuteComplete eventfd write error: ") + strerror(errno));
            }
        }
    }
}

// Reactor 线程消费（业务处理完成）完成队列：匹配 connId 后唤醒对应协程。
// 批量交换队列减少锁竞争，处理僵尸唤醒防止协程泄漏。
void SubReactor::processComplete()
{
    std::queue<std::pair<int, uint64_t>> local;
    {
        std::lock_guard<std::mutex> lock(completeMtx);
        local.swap(completeQueue);
    }
    while (!local.empty())
    {
        auto [fd, connId] = local.front();
        local.pop();

        auto it = conns.find(fd);
        if (it != conns.end() && it->second->id == connId)
        {
            wakeExecuteCoroutine(fd);
            continue;
        }
        // 连接已关闭：检查僵尸唤醒表，防止协程泄漏
        auto zit = zombieWakes.find(connId);
        if (zit != zombieWakes.end())
        {
            auto h = zit->second;
            zombieWakes.erase(zit);
            if (h && !h.done())
                scheduler.schedule(h);
        }
    }
}


// 统一套接字关闭，并维持"只有所属 Reactor 关闭连接"的所有权规则。
void SubReactor::fd_close(int fd, std::string_view reason, bool fromCoroutine)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;

   
     LOG_DEBUG(std::string("[CLOSE] fd=") + std::to_string(fd) + " reason=" + std::string(reason));
    // 优化：加上状态检查，防止当某fd已经关闭之后重复关闭或者关闭之后任然在fd
    if (it->second->state.closed)
        return;

   
    std::coroutine_handle<> coroutineToWake;
    auto h = it->second->session->coroutine_context.handle;
    // 判断是否还在执行
    bool wasExecuting = it->second->session->coroutine_context.state == AwaitType::EXECUTE;
    uint64_t connId = it->second->id;
    if (h)
    {
        it->second->session->coroutine_context.handle = nullptr;
        it->second->session->coroutine_context.state = AwaitType::NONE;

        if (!fromCoroutine && !wasExecuting)
        {
            coroutineToWake = h;
        }else if (!fromCoroutine && wasExecuting)
        {
            // 协程在 Executor 中执行，连接将被删除。
            // 将 handle 存入 zombieWakes，Worker 完成后 processComplete 会通过 connId
            // 找到并调度它，让协程恢复后通过 getConn()==nullptr 安全退出。
            zombieWakes[connId] = h;
        }
        // fromCoroutine: 协程自己调用 fd_close 后会 co_return 自然结束，无需额外操作
    }

    // 删除对应时间轮
    wheel.remove(fd);

    it->second->state.closed = true;

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    conns.erase(it);
    activeConns_.fetch_sub(1, std::memory_order_relaxed);

    // 重新唤醒
    if (coroutineToWake)
    {
        scheduler.schedule(coroutineToWake);
    }
}
// 设置fd为非堵塞，对于新添加的fd都要使用
void SubReactor::fd_unblock(int fd)
{

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
// 重新唤醒、
void SubReactor::rearm(int fd, uint32_t events)
{
    // 加上rearm检查，防止已关闭的fd重新rearm
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    if (it->second->state.closed)
        return;
    epoll_event ev{};
    ev.events = EPOLLONESHOT | EPOLLET | EPOLLRDHUP | events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1)
    {
        LOG_INFO(std::string("epol_ctl MOD failed") + strerror(errno));
    }
}

void SubReactor::run()
{
    // 获得属于自己的线程
    th = std::thread([this]
                     {
            // 放置要处理的业务逻辑函数
            loop(); });
}

RecvState SubReactor::recvSocket(int fd)
{
    // 说明有东西从客户端反过来，准备接受东西，同时将整个任务进行处理

    // 预查询,防止虚空索敌链接幽灵对象
    auto it = conns.find(fd);
    if (it == conns.end())
        return RecvState::CLOSED;

    auto &conn = *it->second;

    // 背压修复处：使用 readableBytes() 代替 buf.size() 判断内存水位
    // readableBytes() 是未读数据大小，buf.size() 是 vector 总容量（含已读和空闲）
    // 原代码用 buf.size() 导致已解析数据仍计入水位，背压判断不准确
    if (conn.readBuffer.readableBytes() > MAX_PENDING_BYTES)
    {
        conn.state.pauseByMemory = true;
        updateEvent(fd);
        return RecvState::PAUSED;
    }
    // 开始接受数据
     while (running.load(std::memory_order_acquire))
    {
        // 性能修复处：recv 缓冲区从 8KB 提升到 64KB
        // 原代码每次最多读 8KB， 请求需要多次 recv 系统调用
        // 64KB 接近 TCP 默认窗口规模，单次 recv 即可读完一个典型请求
        char buffer[65536];
        // 接收消息
        int res = recv(fd, buffer, sizeof(buffer), 0);

        // 进行分类处理判断
        if (res == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // printf("数据已经读完\n");
                break;
            }
            else if (errno == EINTR)
            {
                continue;
            }
            else
            { // 连接失败
                // printf("连接失败\n");
                fd_close(fd, "连接失败", true);
                return RecvState::CLOSED;
            }
        }
        else if (res == 0)
        {
            // TCP FIN 可能和最后一个完整请求同时到达。立即 fd_close 会丢弃已经读入的请求；
            // 记录半关闭状态，让 Session 在能够完整解析时发完最后一个响应。
            conn.state.peerClosed = true;
            break;
        }
        // printf("收到数据：%.*s\n", res, buffer);

        // 开始处理数据
        conn.readBuffer.append(buffer, res);
        conn.pendingBytes = conn.readBuffer.readableBytes();
        if (conn.pendingBytes > MAX_PENDING_BYTES)
        {
            conn.state.pauseByMemory = true;
            updateEvent(fd);
            return RecvState::PAUSED;
        }

        // 遇到有用请求，刷新请求.防止一直接受不到conn
        wheel.refresh(fd);
        // 可优化点：使用零拷贝  std::string localBuf.swap(conns[fd].readBuffer);
        //  或者使用现在的多reactor直接对conns进行操作
    }
    return RecvState::READY;
}


void SubReactor::loop()
{
    // 创建epoll储存大小
    epoll_event events[MAX_EVENTS];
    auto last = std::chrono::steady_clock::now();
    // stop() 会清 running 并写 eventfd；必须在每次 wait 后检查，否则 join() 永久挂起。
    while (running.load(std::memory_order_acquire))
    {
        // 开始监听epfd并将数据存放到events中,优化100ms无连接超时
        // 性能修复处：epoll_wait 超时从 1000ms 降为 100ms
        // 原代码 1000ms 导致新事件最多等 1 秒才被处理，高并发下延迟飙升
        // 100ms 在响应性和 CPU 占用之间取得平衡，同时保证时间轮每秒 tick 精度
        int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
        if (!running.load(std::memory_order_acquire))
            break;
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            LOG_ERROR(std::string("subreactor epoll_wait: ") + strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            // 就处理读出和写入
            if (fd == event_fd)
            {
                uint64_t cnt;
                while (read(event_fd, &cnt, sizeof(cnt)) > 0)
                    ;

                // eventfd 优化处：读完 eventfd 后重置 notified 标志
                // 这样后续的 pushResult/notifyStream/addFd 才能再次触发 eventfd 写入
                // 必须在 read 之后、处理任务之前重置，确保不丢失新入队的任务
                notified.store(false, std::memory_order_release);

                // 段错误修复处：先处理待添加的 fd，再处理任务结果
                // 确保所有对 conns/wheel/epoll_ctl 的操作都在 SubReactor 线程中完成
                processPendingFds();
                processComplete();
            }
            else
            {
                if (events[i].events & EPOLLERR)
                {
                    fd_close(fd, "epoll error");
                    continue;
                }
                // 处理半关闭情况，主要就是防止出现信息还没有发送完直接关闭连接了
                if (events[i].events & (EPOLLHUP | EPOLLRDHUP))
                {
                    auto it = conns.find(fd);
                    if (it != conns.end())
                        it->second->state.peerClosed = true;
                }
                if (events[i].events & EPOLLOUT) //-------处理write-send发出
                {
                    wakeWriteCoroutine(fd);
                }

                if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) // 处理监听接受/半关闭
                {
                    wakeReadCoroutine(fd);
                }
                // scheduler.runReady();
            }
        }

        // 检查超时
        auto now = std::chrono::steady_clock::now();

        auto sec = std::chrono::duration_cast<std::chrono::seconds>(now - last).count(); // 刷新计时
        if (sec >= 1)
        {
            wheel.tick();
            last = now;
        }

        scheduler.runReady();
        // currentTick++;
    }
}

// 段错误修复处：addFd 改为只将 fd 放入待处理队列，由 SubReactor 线程完成实际注册
// 原代码在主线程直接操作 conns/wheel/epoll_ctl，与 SubReactor 线程竞争
// 导致 unordered_map rehash 时迭代器失效 → free(): invalid pointer
void SubReactor::addFd(int fd)
{
    // 判断当前线程是否已经运行
    if (!running.load(std::memory_order_acquire))
    {
        close(fd);
        return;
    }
    // 设置为非堵塞（fcntl 是系统调用，线程安全，可以在主线程做）
    fd_unblock(fd);

    // 将 fd 放入待处理队列
    {
        std::lock_guard<std::mutex> lock(pending_mtx);
        pendingFds.push(fd);
    }

    // eventfd 优化处：使用 atomic<bool> + exchange 合并唤醒
    if (!notified.exchange(true))
    {
        uint64_t one = 1;
        if (write(event_fd, &one, sizeof(one)) == -1)
        {
            if (errno != EAGAIN)
            {
                LOG_INFO(std::string("addFd eventfd write error: ") + strerror(errno));
            }
        }
    }
}

// 段错误修复处：由 SubReactor 线程调用，处理待添加的 fd 队列
// 所有对 conns/wheel/epoll_ctl 的操作都在 SubReactor 线程中完成，消除数据竞争
void SubReactor::processPendingFds()
{
    // 直接交换获得所有数据到本地来慢慢处理
    std::queue<int> local;
    {
        std::lock_guard<std::mutex> lock(pending_mtx);
        local.swap(pendingFds);
    }
    while (!local.empty())
    {
        int fd = local.front();
        local.pop();

        if (!running.load(std::memory_order_acquire))
        {
            close(fd);
            continue;
        }

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

        // 初始化conns对象
        auto conn = std::make_unique<Connection>();

        conn->fd = fd;
        conn->id = ++global_conn_id;
        conn->state.readPaused = false;

        auto *raw = conn.get();
        conns.emplace(fd, std::move(conn));
        activeConns_.fetch_add(1, std::memory_order_relaxed);

        raw->session = std::make_shared<HttpSession>(fd, this);
        // conns[fd] = conn;
        auto task = raw->session->run();
        auto h = task.release();
        raw->session->coroutine_context.handle = h;

        // 设置对应时间轮
        wheel.add(fd);

        scheduler.adopt(fd, h, raw->session);
        scheduler.runReady(); // 启动到第一个co_await
    }
}