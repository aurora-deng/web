
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
    it->second.state.readPaused = it->second.state.pauseByMemory ||
                                  it->second.state.pauseByPipeline ||
                                  it->second.state.pauseByWriteBacklog;
    ev |= EPOLLIN;

    if (it->second.state.wantWrite)
    {
        ev |= EPOLLOUT;
    }
    rearm(fd, ev);
}

// 统一套接字关闭
// 优化：防止其他的线程来杀死我当前线程的fd
void SubReactor::fd_close(int fd, std::string reason)
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
    if (it->second.state.closed)
        return;

    // 删除对应时间轮
    wheel.remove(fd);
    for(auto&[k,v]:it->second.pendingResponses)
    {
        responsePool.release(v.resp);
    }
    it->second.state.closed = true;
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    conns.erase(it);
    // printf("close fd=%d\n", fd);
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
    if (it->second.state.closed)
        return;
    epoll_event ev{};
    ev.events = EPOLLONESHOT | EPOLLET | events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1)
    {
        LOG_INFO(std::string("epol_ctl MOD failed") + strerror(errno));
    }
}

Task SubReactor::session(int fd)
{
    while (true)
    {
        co_await ReadAwaiter(this,fd);
    };
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

    auto &conn = it->second;
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
            conn.nextResponseSeq++;
            continue;
        }
        case SEND_AGAIN:
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

// 读取函数
void SubReactor::handleRead(int fd)
{
    // 说明有东西从客户端反过来，准备接受东西，同时将整个任务进行处理

    // 预查询,防止虚空索敌链接幽灵对象
    auto it = conns.find(fd);
    if (it == conns.end())
        return;

    auto &conn = it->second;
    // 背压修复处：使用 readableBytes() 代替 buf.size() 判断内存水位
    // readableBytes() 是未读数据大小，buf.size() 是 vector 总容量（含已读和空闲）
    // 原代码用 buf.size() 导致已解析数据仍计入水位，背压判断不准确
    if (conn.readBuffer.readableBytes() > MAX_PENDING_BYTES)
    {
        conn.state.pauseByMemory = true;
        updateEvent(fd);
        return;
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
                return;
            }
        }
        else if (res == 0)
        {
            // printf("对端数据已经下线\n");
            fd_close(fd, "对端数据已经下线");
            return;
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

    // 循环防止由于系统内核中相对于所要发送的消息而言内存不够，所以需要使用循环处理httpRequest,防止粘包
    // 优化去掉循环，防止发到被多次调用
    // 优化减轻readBUffer的负担，同时循环处理请求将其塞入到对应的请求队列中
    while (true)
    {
        auto req=requestPool.acquire();
        ParseState state = try_parse_request(conn.readBuffer, *req);
        if (state == PARSE_NEED_MORE)
        {
            requestPool.release(req);
            break;
        }
        if (state == PARSE_ERROR)
        {
            // std::string waning="try_parse_request PARSE_ERROR";
            // LOG_INFO(waning+strerror(errno));
            requestPool.release(req);
            fd_close(fd, "try_parse_request PARSE_ERROR");
            return;
        }

        PendingRequest p;
        p.data = req;
        conn.pendingBytes = conn.readBuffer.readableBytes();
        if (conn.pendingBytes < MAX_PENDING_BYTES)
            conn.state.pauseByMemory = false;

        // 背压修复处：检查 pendingResponses 积压，超限则暂停读
        if (conn.pendingResponses.size() >= MAX_PENDING_RESPONSES)
            conn.state.pauseByWriteBacklog = true;
        p.seq = conn.nextRequestSeq++;
        conn.pendingRequests.push(std::move(p));
    }

    if (!conn.pendingRequests.empty())
    {
        while (!conn.pendingRequests.empty())
            if (!processRequest(fd))
                break;
        updateEvent(fd);
    }
    else
    {
        updateEvent(fd);
    }
    // 更新时间戳

    // if (!closed)
    // {
    //     // 通过状态来处理rearm
    //     // 优化：向find查找，防止生成幽灵fd导致删除其他线程正在进行的fd
    //         if (it->second.state == READING)
    //         {
    //             rearm(fd, EPOLLIN);
    //         }

    // }
}

bool SubReactor::processRequest(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return false;
    auto &conn = it->second;
    if (it->second.state.readPaused)
    {
        return false;
    }
    // 背压修复处：检查 pendingResponses 积压，超限则暂停提交新任务
    // 原代码只检查 inflightTasks，不检查 pendingResponses
    // 如果响应发送慢（客户端接收慢），pendingResponses 会无限积压
    if (conn.pendingResponses.size() >= MAX_PENDING_RESPONSES)
    {
        conn.state.pauseByWriteBacklog = true;
        return false;
    }
    // 优化:防止队列数据过载
    if (conn.inflightTasks >= MAX_PIPELINE)
    {
        conn.state.pauseByPipeline = true;
        return false;
    }
    conn.inflightTasks++;
    // 将处理好的消息取出
    PendingRequest request = conn.pendingRequests.front();
    conn.pendingRequests.pop();

    // _______________________注意___________________________
    conn.pendingBytes = conn.readBuffer.readableBytes();

    if (conn.pendingBytes < MAX_PENDING_BYTES)
        conn.state.pauseByMemory = false;
    uint64_t cid = conn.id;
    // conn.state = PROCESSING;
    SubReactor *reactor = this;

    // 优化:将每个router直接在reactor中构造新的对象，之后通过reactor的构造函数直接构造初始化，防止后续容易出现每次只创建一次router
    // 背压修复处：addTask 返回 false 表示线程池队列满，拒绝任务
    // 此时需要回滚 inflightTasks++，并将请求放回队列
    bool ok = pool.addTask([fd, cid, request, reactor]
                           {
                             TaskResult result;
                             result.fd = fd;
                             result.id = cid;
                             result.seq=request.seq;
                            //  result.state=PROCESSING;
                             // 解析Http
                              // request.data 是从池中获取的 HttpRequest*，直接使用，不再额外 acquire
                             HttpRequest* req = request.data;

                             auto it = req->headers.find("connection");

                            // 通过路由器处理，将req转化成对应的resp
                            auto resp=responsePool.acquire();
                            *resp=reactor->router.handle(*req);

                            

                             // 优化：仔细处理Http/1.0，防止误判keep-alive,防止http1.0直接误判keep-alive
                             if (req->version == "HTTP/1.1")
                             {
                                 // 默认为keep-alive
                                 if (it != req->headers.end() && toLower(it->second) == "close")
                                 {
                                     resp->keepAlive = false;
                                 }
                                 else
                                     resp->keepAlive = true;
                             }
                             else
                             { // HTTP/1.0
                                 // 默认close
                                 if (it != req->headers.end() && toLower(it->second) == "keep-alive")
                                 {
                                     resp->keepAlive = true;
                                 }
                                 else
                                 {
                                     resp->keepAlive = false;
                                 }
                             }

                            
                            resp->buildHeader();
                            result.resp=resp;

                            
                            result.keepAlive=resp->keepAlive;
                            reactor->pushResult(result); 
                            requestPool.release(request.data);
                        });
    if (!ok)
    {
        // 背压修复处：线程池拒绝任务，回滚状态
        conn.inflightTasks--;
        // 将请求放回队列头部
        PendingRequest p;
        p.data = std::move(request.data);
        p.seq = request.seq;
        conn.pendingRequests.push(std::move(p));
        conn.state.pauseByPipeline = true;
        return false;
    }
    return true;
}

void SubReactor::pushResult(ReactorTask res)
{
    {
        std::lock_guard<std::mutex> lock(queue_mtx);

        Task_Queue.push(std::move(res));
    }

    //  通知有东西写入到epoll中
    // 每次都通知，确保不丢失
    // eventfd 优化处：使用 atomic<bool> + exchange 合并唤醒
    // exchange(notified, true)：原子地将 notified 设为 true，返回旧值
    // 如果旧值为 false，说明之前没人通知过，需要写 eventfd
    // 如果旧值为 true，说明已经有人通知过了，跳过写 eventfd
    // 效果：10000 个任务结果只触发 1 次 eventfd 写入
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

// 有新的请求来了就唤醒epollout
bool SubReactor::handleTaskResultOnce()
{
    // 单次畜类每次的worker的返回的结果
    // 等每轮消息处理完之后将信息取出来，减少对锁的持有和竞争

    ReactorTask task;
    // 使用局部作用域，让锁尽快释放
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        if (Task_Queue.empty())
            return false;

        task = std::move(Task_Queue.front());
        Task_Queue.pop();
    }
    // 判断是哪个类型
    // 如果是唤醒
    if (std::holds_alternative<StreamNotify>(task))
    {
        auto &w = std::get<StreamNotify>(task);
        auto it = conns.find(w.fd);
        if (it == conns.end())
            return true;
        if (it->second.id != w.connId)
            return true;
        it->second.state.wantWrite = true;

        updateEvent(w.fd);

        return true;
    }
    auto t = std::get<TaskResult>(task);
    // 优化：同时也是使用find查找，防止高并发导致fd误杀
    auto it = conns.find(t.fd);
    if (it == conns.end())
    {
        // 段错误修复：连接已关闭，释放 resp 避免内存泄漏
        responsePool.release(t.resp);
        return true;
    }
    if (it->second.id != t.id)
    {
        // 段错误修复：连接已关闭，释放 resp 避免内存泄漏
        responsePool.release(t.resp);
        return true;
    }

    bool needRearmRead = false;
    if (it->second.inflightTasks < MAX_PIPELINE && it->second.state.pauseByPipeline)
    {
        it->second.state.pauseByPipeline = false;
        needRearmRead = true;
    }

    // 背压修复处：pendingResponses 积压恢复检查
    // 只有在添加响应后仍然低于水位线时才恢复读
    // 注意：这里先添加响应再检查，所以用 < 而不是 >=
    // （添加后 pendingResponses.size() 会增加 1，所以用 <= MAX_PENDING_RESPONSES 判断）

    // it->second.pendingResponses[task.seq].data = std::move(task.response);
    // 使用零拷贝优化
    // 防止拿到一个空的，使用try_emplace会生成一个pair返回，如果ok=false则说明该返回是原先存在的
    auto [iter, ok] = it->second.pendingResponses.try_emplace(t.seq);
    auto &pending = iter->second;
    // pending.header = std::move(t.header);
    // // 下面三种都是用了共享指针实现零拷贝优化
    // pending.body = std::move(t.body);
    pending.resp=t.resp;

    // 将信息拆分之后返回个conns并更新conns的状态
    it->second.state.wantWrite = true;
    it->second.keepAlive = t.keepAlive;

    // 背压修复处：添加响应后检查写积压水位
    // 如果添加后超过水位线，设置 pauseByWriteBacklog 暂停读
    // 如果添加后低于水位线的一半，恢复 pauseByWriteBacklog
    if (it->second.pendingResponses.size() > MAX_PENDING_RESPONSES)
    {
        it->second.state.pauseByWriteBacklog = true;
    }
    else if (it->second.pendingResponses.size() < MAX_PENDING_RESPONSES / 2)
    {
        if (it->second.state.pauseByWriteBacklog)
        {
            it->second.state.pauseByWriteBacklog = false;
            needRearmRead = true;
        }
    }

    auto chunk = std::dynamic_pointer_cast<ChunkedBody>(pending.resp->body);
    if (chunk)
    {
        chunk->wakeup = [reactor = this, fd = t.fd, cid = t.id]
        {
            reactor->notifyStream(fd, cid);
        };
    }

    // 背压修复处：needRearmRead 表示背压恢复，需要重新注册 EPOLLIN
    // pendingResponses.size() == 1 表示之前没有待发送响应，需要注册 EPOLLOUT
    // 两者任一满足都需要更新事件
    if (needRearmRead || it->second.pendingResponses.size() == 1)
    {
        it->second.state.wantWrite = true;
        updateEvent(t.fd);
    }

    // 背压修复处：背压恢复后，主动处理 pendingRequests 中的积压请求
    // 因为之前暂停读时，pendingRequests 中可能还有未提交的请求
    if (needRearmRead && !it->second.pendingRequests.empty())
    {
        while (!it->second.pendingRequests.empty())
            if (!processRequest(t.fd))
                break;
        updateEvent(t.fd);
    }

    // 防空指针
    size_t bodySize = 0;
    if (pending.resp->body)
    {
        bodySize = pending.resp->body->memoryUsage();
    }
    // 优化;防爆
    if (bodySize + pending.resp->HeaderBody_->memoryUsage() > MB(4))
    {
        fd_close(t.fd, "response overflow");
        return false;
    }

    return true;
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
                while (handleTaskResultOnce() && ++processed < 256)
                    ;
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
                    handleRead(fd);
                }
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
        Connection conn;
        auto task=session(fd);
        task.resume();
        conn.fd = fd;
        conn.id = ++global_conn_id;
        // keepAlive 默认 true（在 Connection 结构体中初始化）
        // 当任务结果返回时，handleTaskResultOnce 会根据 HTTP 版本和 Connection 头正确设置
        conn.state.readPaused = false;
        conns[fd] = conn;

        // 设置对应时间轮
        wheel.add(fd);
    }
}