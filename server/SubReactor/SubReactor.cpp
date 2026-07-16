
#include "SubReactor.h"
std::atomic<uint64_t> global_conn_id{0}; // 自增

// 统一小写
static inline std::string toLower(std::string s)
{
    for (char &c : s)
        c = std::tolower((unsigned char)c);
    return s;
}

void SubReactor::updateEvent(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    uint32_t ev = 0;
    // 背压修复处：综合所有背压条件决定是否暂停读
    // 任一背压条件触发都应暂停读，防止数据继续涌入

    it->second->state.readPaused = it->second->state.pauseByMemory ;
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
    if (ctx.state!=AwaitType::READ)
        return; // 已在运行/已唤醒，不重复入队
    if (!ctx.handle || ctx.handle.done())
        return; // 无效 handle，跳过
    // 刷新状态
    ctx.state = AwaitType::NONE;
    ctx.waiting=false;
    auto h = ctx.handle;
    if (h)
    {
        scheduler.add(h);
    }
}

// 修复3：安全唤醒写协程，防止重复 scheduler.add
void SubReactor::wakeWriteCoroutine(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    auto &ctx = it->second->session->coroutine_context;
    if (ctx.state!=AwaitType::WRITE)
        return;
    if (!ctx.handle || ctx.handle.done())
        return;
    ctx.state = AwaitType::NONE;
    ctx.waiting=false;
     auto h = ctx.handle;
    if (h)
    {
        scheduler.add(h);
    }
}
// 统一套接字关闭
// 优化：防止其他的线程来杀死我当前线程的fd
void SubReactor::fd_close(int fd, std::string reason, bool fromCoroutine)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;

    // 优化： 段错误修复处：移除误导性的 strerror(errno)，errno 可能是上一次系统调用残留的值
    // "Resource temporarily unavailable" 就是残留的 EAGAIN，与关闭操作无关
    // 性能修复处：原代码每个连接关闭都 cout + endl，wrk 结束时 1000 连接同时关闭 → cout 锁串行化
    // 改用异步 Logger，不阻塞 SubReactor 线程
    LOG_DEBUG(std::string("[CLOSE] fd=") + std::to_string(fd) + " reason=" + reason);
    // 优化：加上状态检查，防止当某fd已经关闭之后重复关闭或者关闭之后任然在fd
    if (it->second->state.closed)
        return;

  
    // 协程安全修复：区分「协程内关闭」和「外部关闭」
    // 关键原则：绝不在 fd_close 中 h.destroy() 协程
    // 因为 readyQueue 中可能还有该 handle，destroy 后 resume 是 UB
    // 方案：唤醒协程让它自行 co_return，final_suspend(suspend_never) 后帧自动释放
    std::coroutine_handle<> coroutineToWake;
    auto h = it->second->session->coroutine_context.handle;
    if (h)
    {
        it->second->session->coroutine_context.handle = nullptr;
        it->second->session->coroutine_context.state=AwaitType::NONE;

        if (!fromCoroutine && !h.done())
        {
            // 外部关闭：唤醒挂起中的协程，让它检测到 closed 后自行 co_return
            // 不能在 conns.erase 之前 resume（否则协程可能访问正在被删除的 conn）
            // 只记录 handle，等 conns.erase 之后再 add 到 readyQueue
            coroutineToWake = h;
        }
        // fromCoroutine: 协程自己调用 fd_close 后会 co_return 自然结束，无需额外操作
    }

    // 删除对应时间轮
    wheel.remove(fd);
   
    it->second->state.closed = true;

    scheduler.cancel(fd);       // 清除 waiting 中的关联
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    conns.erase(it);
    // printf("close fd=%d\n", fd);
    // 在 conns.erase 之后唤醒协程（此时 conns[fd] 已不存在）
    // 协程 resume 后 session 中 conns.find(fd) 返回 end() → co_return
    // final_suspend(suspend_never) → 帧自动释放，无需外部 destroy
    if (coroutineToWake)
    {
        scheduler.add(coroutineToWake);
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
    ev.events = EPOLLONESHOT | EPOLLET | events;
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
    th.detach(); // 使用完之后删除该线程
}

// 优化，通过response来返回对应返回
SendState SubReactor::sendBody(int fd, HttpResponse &resp)
{

    while (true)
    {

        // header:先保证header能够发完

        if (resp.HeaderBody_->remain() > 0)
        {

            // 构建iov
            Block *block = segPool.acquire();
            // 由于后面存在判断会导致直接返回，所以创建变量BlockGuard随着函数的消失而自动析构释放block
            BlockGuard guard{segPool, block};

            // 性能修复处：用栈上 iovec[64] 替代 std::vector<iovec>，消除每次 writev 的堆分配
            if (!resp.HeaderBody_->buildSegments(block, 65536))
            {
                return resp.HeaderBody_->finished() ? SEND_OK : SEND_AGAIN;
            }
            iovec vec[64];
            int cnt = BlockToIov(block, vec, 64);
            int n = writev(fd, vec, cnt);
            if (n > 0) // 清空已经发送的部分
            {
                wheel.refresh(fd);
                resp.HeaderBody_->consume(n);
                // 最终header判定
                if (resp.HeaderBody_->remain())
                    return SEND_AGAIN;

                // 用于处理正常发送后跳过SEND_Header_CLOSED
                continue;
            }
            else if (n == -1) // 表示没有消息或者发送的消息发布完了
            {
                LOG_ERROR(std::string("send error: ") + strerror(errno));
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // 发不完等下一次
                    return SEND_AGAIN;
                }
                else if (errno == EINTR)
                {
                    // 信号被打断重新尝试
                    continue;
                }
            }
            LOG_ERROR(std::string("SEND_Header error: ") + strerror(errno));

            return SEND_Header_CLOSED;
        }

        // body
        if (!resp.body)
        {
            return SEND_OK;
        }
        // 判断是否为发送文件
        // auto file = std::dynamic_pointer_cast<FileBody>(resp.body);
        if (resp.body->useSendfile())
        {
            auto n = resp.body->sendFile(fd);
            if (n > 0)
            {
                wheel.refresh(fd);
                if (resp.body->finished())
                    return SEND_OK;
                // 发了但没有发完
                return SEND_AGAIN;
            }

            if (n == -2)
                return SEND_AGAIN;

            return SEND_File_CLOSED;
        }
        // 构建iov
        Block *block = segPool.acquire();
        // 由于后面存在判断会导致直接返回，所以创建变量BlockGuard随着函数的消失而自动析构释放block
        BlockGuard guard{segPool, block};

        // 性能修复处：用栈上 iovec[64] 替代 std::vector<iovec>，消除每次 writev 的堆分配
        if (!resp.body->buildSegments(block, 65536))
        {
            return resp.body->finished() ? SEND_OK : SEND_AGAIN;
        }
        iovec vec[64];
        int cnt = BlockToIov(block, vec, 64);
        int n = writev(fd, vec, cnt);
        if (n > 0)
        {

            wheel.refresh(fd);
            resp.body->consume(n);
            if (resp.body->finished())
                return SEND_OK;
            continue;
        }
        else if (n == -1) // 表示没有消息或者发送的消息发布完了
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 发不完等下一次
                return SEND_AGAIN;
            }
            else if (errno == EINTR)
            {
                // 信号被打断重新尝试
                continue;
            }
            else
            {
                // 出现错误
                LOG_ERROR(std::string("error: ") + strerror(errno));
                return SEND_Writev_CLOSED;
            }
        }
        else if (n == 0)
        {
            // 表示连接异常直接关闭即可
            LOG_ERROR(std::string("connect error: ") + strerror(errno));
            return SEND_Writev_CLOSED;
        }
    }
}


bool SubReactor::recvSocket(int fd)
{
    // 说明有东西从客户端反过来，准备接受东西，同时将整个任务进行处理

    // 预查询,防止虚空索敌链接幽灵对象
    auto it = conns.find(fd);
    if (it == conns.end())
        return;

    auto &conn = *it->second;

    // 背压修复处：使用 readableBytes() 代替 buf.size() 判断内存水位
    // readableBytes() 是未读数据大小，buf.size() 是 vector 总容量（含已读和空闲）
    // 原代码用 buf.size() 导致已解析数据仍计入水位，背压判断不准确
    if (conn.readBuffer.readableBytes() > MAX_PENDING_BYTES)
    {
        conn.state.pauseByMemory = true;
        updateEvent(fd);
        return false;
    }
    // 开始接受数据
    while (true)
    {
        // 性能修复处：recv 缓冲区从 8KB 提升到 64KB
        // 原代码每次最多读 8KB，大请求或 pipeline 请求需要多次 recv 系统调用
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
                fd_close(fd, "连接失败");
                return false;
            }
        }
        else if (res == 0)
        {
            // printf("对端数据已经下线\n");
            fd_close(fd, "对端数据已经下线");
            return false;
        }
        // printf("收到数据：%.*s\n", res, buffer);

        // 开始处理数据
        conn.readBuffer.append(buffer, res);
        conn.pendingBytes = conn.readBuffer.readableBytes();

        // 遇到有用请求，刷新请求.防止一直接受不到conn
        wheel.refresh(fd);
        // 可优化点：使用零拷贝  std::string localBuf.swap(conns[fd].readBuffer);
        //  或者使用现在的多reactor直接对conns进行操作
    }
    return true;
}


bool SubReactor::parseOneRequest(HttpRequest &req, Connection &conn)
{
    ParseState state = try_parse_request(conn.readBuffer, req);
    if (state == PARSE_NEED_MORE)
    {
        return false;
    }

    if (state == PARSE_OK)
    {
        conn.pendingBytes = conn.readBuffer.readableBytes();
        if (conn.pendingBytes < MAX_PENDING_BYTES)
            conn.state.pauseByMemory = false;
        // 背压修复处：检查 pendingResponses 积压，超限则暂停读
        
        return true;
    }

    fd_close(conn.fd, "parse error");
    return false;
}



HttpResponse *SubReactor::createResponse(HttpRequest &req)
{
    auto resp = responsePool.acquire();
    *resp = router.handle(req);
    return resp;
}

void SubReactor::finishReaponse()
{
}


void SubReactor::loop()
{
    // 创建epoll储存大小
    epoll_event events[MAX_EVENTS];
    auto last = std::chrono::steady_clock::now();
    while (true)
    {
        // 开始监听epfd并将数据存放到events中,优化100ms无连接超时
        // 性能修复处：epoll_wait 超时从 1000ms 降为 100ms
        // 原代码 1000ms 导致新事件最多等 1 秒才被处理，高并发下延迟飙升
        // 100ms 在响应性和 CPU 占用之间取得平衡，同时保证时间轮每秒 tick 精度
        int n = epoll_wait(epfd, events, MAX_EVENTS, 100);

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
                
            }
            else
            {
                if (events[i].events & EPOLLOUT) //-------处理write-send发出
                {
                    wakeWriteCoroutine(fd);
                }

                if (events[i].events & EPOLLIN) // 处理监听接受
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
    while (true)
    {
        int fd;
        {
            std::lock_guard<std::mutex> lock(pending_mtx);
            if (pendingFds.empty())
                break;
            fd = pendingFds.front();
            pendingFds.pop();
        }

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);

        // 初始化conns对象
        auto conn = std::make_unique<Connection>();

        conn->fd = fd;
        conn->id = ++global_conn_id;
        conn->state.readPaused = false;

        auto *raw = conn.get();
        conns.emplace(fd, std::move(conn));
        
        raw->session = std::make_shared<HttpSession>(fd, this);
        // conns[fd] = conn;
        auto task = raw->session->run();    
        auto h=task.release();
        raw->session->coroutine_context.handle=h;
        
        scheduler.add(h); // handle交给scheduler管理
        scheduler.runReady();          // 启动到第一个co_await
        // 设置对应时间轮
        wheel.add(fd);
    }
}