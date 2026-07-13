
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

    it->second->state.readPaused = it->second->state.pauseByMemory ||
                                   it->second->state.pauseByPipeline ||
                                   it->second->state.pauseByWriteBacklog;
    if (!it->second->state.readPaused)
        ev |= EPOLLIN;

    if (it->second->state.wantWrite)
    {
        ev |= EPOLLOUT;
    }
    // 性能优化：缓存当前注册的事件，只在变化时才调用 epoll_ctl
    auto evit = registeredEvents.find(fd);
    if (evit != registeredEvents.end() && evit->second == ev)
        return; // 事件未变化，跳过

    rearm(fd, ev);
    registeredEvents[fd] = ev;
}

void SubReactor::wakeReadCoroutine(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    auto &c = *it->second;
    auto &ctx = c.session->coroutine_context;
    if (!ctx.waiting)
        return; // 已在运行/已唤醒，不重复入队
    if (!ctx.handle || ctx.handle.done())
        return; // 无效 handle，跳过
    ctx.state = AwaitType::READ;
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
    auto &c = *it->second;
    if (!c.coroutine_context.waitingWrite)
        return;
    if (!c.coroutine_context.handle || c.coroutine_context.handle.done())
        return;
    c.coroutine_context.waitingWrite = false;
    scheduler.add(c.coroutine_context.handle);
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

    // // 销毁协程，防止泄漏

    // if (h)
    // {
    //     it->second.coroutine.handle = nullptr;
    //     if (!h.done()) h.destroy();
    // }
    // 协程通弄过co_retrun来自动销毁，不靠destroy

    // 协程安全修复：区分「协程内关闭」和「外部关闭」
    // 关键原则：绝不在 fd_close 中 h.destroy() 协程
    // 因为 readyQueue 中可能还有该 handle，destroy 后 resume 是 UB
    // 方案：唤醒协程让它自行 co_return，final_suspend(suspend_never) 后帧自动释放
    std::coroutine_handle<> coroutineToWake;
    auto h = it->second->coroutine_context.handle;
    if (h)
    {
        it->second->coroutine_context.handle = nullptr;
        it->second->coroutine_context.waitingRead = false;
        it->second->coroutine_context.waitingWrite = false;

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
    for (auto &[k, v] : it->second->pendingResponses)
    {
        responsePool.release(v.resp);
    }
    it->second->state.closed = true;
    registeredEvents.erase(fd); // 清除事件缓存
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

// void SubReactor::wakeReadCoroutine(int fd, Handle handle)
// {

//     auto it=conns.find(fd);
//     if(it==conns.end())return;
//     auto &c=it->second;
//     if(!c.coroutine.waitingRead)return;                 //已运行/已经唤醒，不重复入队
//     if(!c.coroutine.handle||c.coroutine.handle.done()) return;
//     c.coroutine.waitingRead=false;
//     scheduler.add(handle);
// }

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
SendState SubReactor::sendBody(int fd, pendingResponse &resp)
{

    while (true)
    {

        // header:先保证header能够发完

        if (resp.resp->HeaderBody_->remain() > 0)
        {

            // 构建iov
            Block *block = segPool.acquire();
            // 由于后面存在判断会导致直接返回，所以创建变量BlockGuard随着函数的消失而自动析构释放block
            BlockGuard guard{segPool, block};

            // 性能修复处：用栈上 iovec[64] 替代 std::vector<iovec>，消除每次 writev 的堆分配
            if (!resp.resp->HeaderBody_->buildSegments(block, 65536))
            {
                return resp.resp->HeaderBody_->finished() ? SEND_OK : SEND_AGAIN;
            }
            iovec vec[64];
            int cnt = BlockToIov(block, vec, 64);
            int n = writev(fd, vec, cnt);
            if (n > 0) // 清空已经发送的部分
            {
                wheel.refresh(fd);
                resp.resp->HeaderBody_->consume(n);
                // 最终header判定
                if (resp.resp->HeaderBody_->remain())
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
        if (!resp.resp->body)
        {
            return SEND_OK;
        }
        // 判断是否为发送文件
        // auto file = std::dynamic_pointer_cast<FileBody>(resp.body);
        if (resp.resp->body->useSendfile())
        {
            auto n = resp.resp->body->sendFile(fd);
            if (n > 0)
            {
                wheel.refresh(fd);
                if (resp.resp->body->finished())
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
        if (!resp.resp->body->buildSegments(block, 65536))
        {
            return resp.resp->body->finished() ? SEND_OK : SEND_AGAIN;
        }
        iovec vec[64];
        int cnt = BlockToIov(block, vec, 64);
        int n = writev(fd, vec, cnt);
        if (n > 0)
        {

            wheel.refresh(fd);
            resp.resp->body->consume(n);
            if (resp.resp->body->finished())
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

// 写入函数
void SubReactor::handleWrite(int fd)
{
    // 优化，---加一层检查防止对于发到不存在直接创建出来一个新的
    auto it = conns.find(fd);
    if (it == conns.end())
        return;

    auto &conn = *it->second;
    while (true)
    {
        auto iter = conn.pendingResponses.find(conn.nextResponseSeq);
        if (iter == conn.pendingResponses.end()) // 表示当前该fd没有可以发送respons,切换为监听状态
        {
            break;
        }
        // 按照顺序取出消息并回复
        auto &resp = iter->second;

        // ——————————————————————————————————————————————————————————————————日志-------------
        // if (
        //     !resp.resp->body)
        // {
        //     fd_close(fd, "RESP BODY NULL ");
        //     return;
        // }
        // 段错误修复：body 为 null 是合法的（如 204/304 响应），
        // sendBody 内部已处理 body==null 的情况（返回 SEND_OK），无需特殊处理

        switch (sendBody(fd, resp))
        {
        case SEND_OK:
        {
            if (conn.inflightTasks > 0)
                conn.inflightTasks--;
            responsePool.release(resp.resp);
            conn.pendingResponses.erase(iter);
            continue;
        }
        case SEND_AGAIN:
            // 非 协程函数 中不能 co_await，改为注册 EPOLLOUT 等下次可写
            conn.state.wantWrite = true;
            break;
        case SEND_Header_CLOSED:
            fd_close(fd, "sendHeader close");
            return;
        case SEND_Writev_CLOSED:
            fd_close(fd, "sendwritev close");
            return;
        case SEND_Chunk_CLOSED:
            fd_close(fd, "sendChunk error");
            return;
        case SEND_EndChunk_CLOSED:
            fd_close(fd, "sendEndChunk close");
            return;
        case SEND_File_CLOSED:
            fd_close(fd, "sendHeader close");
            return;
        }
        break;
    }

    // 段错误修复处：循环结束后重新查找 conn，因为循环内可能已通过 fd_close 删除了 conn
    it = conns.find(fd);
    if (it == conns.end())
        return;
    // 重新获取 conn 引用（循环前的 conn 可能因 fd_close 而悬空）
    auto &conn_after_loop = it->second;

    // 背压修复处：发送完响应后检查是否可以恢复读
    // 如果 pendingResponses 降到水位线以下，恢复 pauseByWriteBacklog
    if (conn_after_loop.state.pauseByWriteBacklog &&
        conn_after_loop.pendingResponses.size() < MAX_PENDING_RESPONSES / 2)
    {
        conn_after_loop.state.pauseByWriteBacklog = false;
    }
    // 背压修复处：发送完响应后检查是否可以恢复读
    // 如果 readBuffer 可读数据降到水位线以下，恢复 pauseByMemory
    if (conn_after_loop.state.pauseByMemory &&
        conn_after_loop.readBuffer.readableBytes() < MAX_PENDING_BYTES / 2)
    {
        conn_after_loop.state.pauseByMemory = false;
    }
    // 背压修复处：发送完响应后检查是否可以恢复提交任务
    // 如果 inflightTasks 降到水位线以下，恢复 pauseByPipeline
    if (conn_after_loop.state.pauseByPipeline &&
        conn_after_loop.inflightTasks < MAX_PIPELINE)
    {
        conn_after_loop.state.pauseByPipeline = false;
    }

    // 将这一部分提出循环之外，放在循环内会重复调用浪费时间
    if (conn_after_loop.pendingResponses.empty())
    {

        // 优化防止直接结束fd之后又要重新连接，直接更改模式
        conn_after_loop.state.wantWrite = false;
        // 崩溃修复处：只在所有响应都发完且没有进行中的任务时才检查 keepAlive
        // 原代码在 pendingResponses 为空时就检查 keepAlive，但此时可能还有任务在进行中
        // inflightTasks > 0 表示还有任务在线程池中处理，结果还没回来
        // 此时 keepAlive 可能还没被最终设置，不应该关闭连接
        if (conn_after_loop.inflightTasks > 0)
        {
            // 还有任务在进行中，等待结果回来后再决定
            updateEvent(fd);
        }
        else if (conn_after_loop.keepAlive)
        {
            if (!conn_after_loop.pendingRequests.empty())
            {
                while (!conn_after_loop.pendingRequests.empty())
                    if (!processRequest(fd))
                        break;
            }
            // 重新激活为监听状态
            updateEvent(fd);
        }
        else
        {
            fd_close(fd, "conn_after_loop.keepAlive false");
        }
    }
    else // 处理sent_again
    {
        conn_after_loop.state.wantWrite = true;
        updateEvent(fd);
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
        if (conn.pendingResponses.size() >= MAX_PENDING_RESPONSES)
            conn.state.pauseByWriteBacklog = true;
        return true;
    }

    fd_close(conn.fd, "parse error");
    return false;
}

// 读取函数
// handleRead 负责“唤醒协程”，HttpSession::run负责“描述一次连接完整生命周期”。
void SubReactor::handleRead(int fd)
{
    scheduler.resume(fd, EPOLLIN);
    scheduler.runReady();
}

Task<HttpResponse *> SubReactor::execute(HttpRequest &req)
{
    auto it = req.headers.find("connection");

    auto resp = co_await router.handle(req);
    // 优化：仔细处理Http/1.0，防止误判keep-alive,防止http1.0直接误判keep-alive
    if (req.version == "HTTP/1.1")
    {
        // 默认为keep-alive
        if (it != req.headers.end() && toLower(it->second) == "close")
        {
            resp.keepAlive = false;
        }
        else
            resp.keepAlive = true;
    }
    else
    { // HTTP/1.0
        // 默认close
        if (it != req.headers.end() && toLower(it->second) == "keep-alive")
        {
            resp.keepAlive = true;
        }
        else
        {
            resp.keepAlive = false;
        }
    }

    resp.buildHeader();
    co_return resp;
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

void SubReactor::notifyStream(int fd, uint64_t id)
{
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        Task_Queue.push(StreamNotify{fd, id});
    }

    // eventfd 优化处：同 pushResult，使用 atomic<bool> + exchange 合并唤醒
    if (!notified.exchange(true))
    {
        uint64_t one = 1;
        if (write(event_fd, &one, sizeof(one)) == -1)
        {
            if (errno != EAGAIN)
            {
                LOG_INFO(std::string("eventfd write") + strerror(errno));
            }
        }
    }
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
                // 性能修复处：限制每轮最多处理 256 个任务结果，避免长时间阻塞事件循环
                // 原代码 while(handleTaskResultOnce()) 无限循环，大量结果时新连接/读事件得不到处理
                // 导致 75% 延迟飙到 2289ms，剩余结果下轮继续处理
                int processed = 0;

                // 崩溃修复处：如果还有剩余结果，重新写 eventfd 通知自己下轮继续处理
                // 原代码只处理 256 个就退出，剩余结果没有通知永远不会被处理
                // 导致客户端收不到响应超时关闭，服务器 conns 积压最终崩溃
                {
                    bool hasMore = false;
                    {
                        std::lock_guard<std::mutex> lock(queue_mtx);
                        hasMore = !Task_Queue.empty();
                    }
                    if (hasMore)
                    {
                        // eventfd 优化处：使用 notified 标志合并唤醒
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
                }
            }
            else
            {
                if (events[i].events & EPOLLOUT) //-------处理write-send发出
                {
                    handleWrite(fd);
                }

                if (events[i].events & EPOLLIN) // 处理监听接受
                {
                    wakeReadCoroutine(fd);
                }
                scheduler.runReady();
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
        auto ptr = conn.get();
        conns.emplace(fd, std::move(conn));
        auto &realConn = *conns[fd];
        // keepAlive 默认 true（在 Connection 结构体中初始化）
        // 当任务结果返回时，handleTaskResultOnce 会根据 HTTP 版本和 Connection 头正确设置
        conn->state.readPaused = false;
        realConn.session = std::make_unique<HttpSession>(this, &realConn);
        // conns[fd] = conn;
        auto task = realConn.session->run();
        auto h=task.release();
        realConn.session->coroutine_context.handle=h;
        // conns入库之后，将其对应连接的对应协程唤醒
        // 必须要使用release从而实现所有权的转移
        scheduler.add(h); // handle交给scheduler管理
        scheduler.runReady();          // 启动到第一个co_await
        // 设置对应时间轮
        wheel.add(fd);
    }
}