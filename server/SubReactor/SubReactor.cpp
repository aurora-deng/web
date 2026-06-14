#include "server/SubReactor/SubReactor.h"
#include <fcntl.h>
#include <atomic>
#include <string.h>
#include "server/http/http.h"
#include "server/Route/Router.h"
#include "log/logger/logger.h"
#include <sys/uio.h>
#include <algorithm>
#include <sys/sendfile.h>
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
    it->second.state.readPaused = it->second.state.pauseByMemory || it->second.state.pauseByPipeline;
    if (!it->second.state.readPaused)
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
    std::cout
        << "[CLOSE]"
        << " fd="
        << fd
        << " reason="
        << reason
        << std::endl;

    // 优化：加上状态检查，防止当某fd已经关闭之后重复关闭或者关闭之后任然在fd
    if (it->second.state.closed)
        return;

    // 删除对应时间轮
    wheel.remove(fd);

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
        if (resp.headerOffset < resp.header.size())
        {
            int n = send(fd, resp.header.data() + resp.headerOffset, resp.header.size() - resp.headerOffset, MSG_NOSIGNAL);
            if (n > 0) // 清空已经发送的部分
            {
                Conn timerconn;
                timerconn.fd = fd;
                wheel.refresh(timerconn);
                resp.headerOffset += n;
                // 最终header判定
                if (resp.headerOffset < resp.header.size())
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
            return SEND_Header_CLOSED;
        }

        // body
        if (!resp.body)
        {
            return SEND_OK;
        }
        // 判断是否为发送文件
        auto file = std::dynamic_pointer_cast<FileBody>(resp.body);
        if (file)
        {
            auto n = file->sendFile(fd);
            if (n > 0)
            {
                Conn timerconn;
                timerconn.fd = fd;
                wheel.refresh(timerconn);
                if (file->finished())
                    return SEND_OK;
                // 发了但没有发完
                return SEND_AGAIN;
            }

            if (n == -2)
                return SEND_AGAIN;

            return SEND_File_CLOSED;
        }
        // 构建iov
        std::vector<iovec> vec;
        if (!resp.body->buildIov(vec, 65536))
        {
            return resp.body->finished() ? SEND_OK : SEND_AGAIN;
        }

        int n = writev(fd, vec.data(), vec.size());
        if (n > 0)
        {
            Conn t;
            t.fd = fd;
            wheel.refresh(t);
            resp.body->consume(n);
            if (resp.body->finished())
                return SEND_OK;
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
        if (
            !resp.body)
        {
            fd_close(fd, "RESP BODY NULL ");
            return;
        }

        switch (sendBody(fd, resp))
        {
        case SEND_OK:
        {
            if (conn.inflightTasks > 0)
                conn.inflightTasks--;
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

    // 将这一部分提出循环之外，放在循环内会重复调用浪费时间
    if (conn_after_loop.pendingResponses.empty())
    {

        // 优化防止直接结束fd之后又要重新连接，直接更改模式
        conn_after_loop.state.wantWrite = false;
        if (conn_after_loop.keepAlive)
        {
            if (!conn_after_loop.pendingRequests.empty())
            {
                processRequest(fd);
            }
            // 重新激活为监听状态
            updateEvent(fd);
        }
        else
        {
            // printf("conn_after_loop.keepAlive false\n");
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

    // 标志：用于判断是否需要进行重新唤醒，同时还使用epollONeshot防止多次提醒
    bool closed = false;

    // 预查询,防止虚空索敌链接幽灵对象
    auto it = conns.find(fd);
    if (it == conns.end())
        return;

    auto &conn = it->second;
    // // 优化：判断是否可以进行数据读入,不单纯使用PROCESSING，可以防止readBuffer越积越大
    if (conn.pendingBytes > MAX_PENDING_BYTES)
    {
        conn.state.pauseByMemory = true;
        updateEvent(fd);
        return;
    }
    // 开始接受数据
    while (true)
    {
        char buffer[8192];
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
                closed = true; // 更新close用于后面重新唤醒
                break;
            }
        }
        else if (res == 0)
        {
            // printf("对端数据已经下线\n");
            fd_close(fd, "对端数据已经下线");
            closed = true;
            break;
        }
        // printf("收到数据：%.*s\n", res, buffer);

        // 开始处理数据
        conn.readBuffer.append(buffer, res);
        conn.pendingBytes = conn.readBuffer.buf.size();

        // 遇到有用请求，刷新请求.防止一直接受不到conn
        Conn timeconn;
        timeconn.fd = fd;
        wheel.refresh(timeconn);
        // 可优化点：使用零拷贝  std::string localBuf.swap(conns[fd].readBuffer);
        //  或者使用现在的多reactor直接对conns进行操作
    }

    if (closed)
        return;

    // 循环防止由于系统内核中相对于所要发送的消息而言内存不够，所以需要使用循环处理httpRequest,防止粘包
    // 优化去掉循环，防止发到被多次调用
    // 优化减轻readBUffer的负担，同时循环处理请求将其塞入到对应的请求队列中
    while (true)
    {
        HttpRequest req;
        ParseState state = try_parse_request(conn.readBuffer, req);
        if (state == PARSE_NEED_MORE)
            break;
        if (state == PARSE_ERROR)
        {
            // std::string waning="try_parse_request PARSE_ERROR";
            // LOG_INFO(waning+strerror(errno));
            fd_close(fd, "try_parse_request PARSE_ERROR");
            return;
        }

        PendingRequest p;
        p.data = std::move(req);
        conn.pendingBytes = conn.readBuffer.buf.size();
        if (conn.pendingBytes < MAX_PENDING_BYTES)
            conn.state.pauseByMemory = false;
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
    conn.pendingBytes = conn.readBuffer.buf.size();

    if (conn.pendingBytes < MAX_PENDING_BYTES)
        conn.state.pauseByMemory = false;
    uint64_t cid = conn.id;
    // conn.state = PROCESSING;
    SubReactor *reactor = this;

    // 优化:将每个router直接在reactor中构造新的对象，之后通过reactor的构造函数直接构造初始化，防止后续容易出现每次只创建一次router
    pool.addTask([fd, cid, request, reactor]
                 {
                             TaskResult result;
                             result.fd = fd;
                             result.id = cid;
                             result.seq=request.seq;
                            //  result.state=PROCESSING;
                             // 解析Http
                             HttpRequest req =request.data;

                             auto it = req.headers.find("connection");

                            // 通过路由器处理，将req转化成对应的resp
                            HttpResponse resp=reactor->router.handle(req);

                            

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

                            
                            
                             result.header=resp.buildHeader();
                             result.body=resp.body;
                            
                             result.keepAlive=resp.keepAlive;
                             reactor->pushResult(result); });
    return true;
}

void SubReactor::pushResult(ReactorTask res)
{
    bool needWalk = false;
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        if (Task_Queue.empty())
        {
            needWalk = true;
        }
        Task_Queue.push(std::move(res));
    }

    if (needWalk)
    {
        //  通知有东西写入到epoll中
        uint64_t one = 1;
        if (write(event_fd, &one, sizeof(one)) == -1)
        {
            // 失败处理
            if (errno != EAGAIN)
            {
                LOG_INFO(std::string("eventfd write") + strerror(errno));
            }
        }
    }
}

void SubReactor::notifyStream(int fd, uint64_t id)
{
    bool needWalk = false;
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        if (Task_Queue.empty())
        {
            needWalk = true;
        }
        Task_Queue.push(StreamNotify{fd, id});
    }

    if (needWalk)
    {
        //  通知有东西写入到epoll中
        uint64_t one = 1;
        if (write(event_fd, &one, sizeof(one)) == -1)
        {
            // 失败处理
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
        return true;
    if (it->second.id != t.id)
        return true;

    bool needRearmRead = false;
    if (it->second.inflightTasks < MAX_PIPELINE && it->second.state.pauseByPipeline)
    {
        it->second.state.pauseByPipeline = false;
        needRearmRead = true;
    }

    // it->second.pendingResponses[task.seq].data = std::move(task.response);
    // 使用零拷贝优化
    // 防止拿到一个空的，使用try_emplace会生成一个pair返回，如果ok=false则说明该返回是原先存在的
    auto [iter, ok] = it->second.pendingResponses.try_emplace(t.seq);
    auto &pending = iter->second;
    pending.header = std::move(t.header);
    // 下面三种都是用了共享指针实现零拷贝优化
    pending.body = std::move(t.body);

    // 将信息拆分之后返回个conns并更新conns的状态
    it->second.state.wantWrite = true;
    it->second.keepAlive = t.keepAlive;

    auto chunk = std::dynamic_pointer_cast<ChunkedBody>(pending.body);
    if (chunk)
    {
        chunk->wakeup = [reactor = this, fd = t.fd, cid = t.id]
        {
            reactor->notifyStream(fd, cid);
        };
    }

    // 如果之前没有需要写的，则需要重新唤醒对应fd为epollout状态
    if (needRearmRead || it->second.pendingResponses.size() == 1)
    {
        it->second.state.wantWrite = true;
        updateEvent(t.fd);
    }
    // 防空指针
    size_t bodySize = 0;
    if (pending.body)
    {
        bodySize = pending.body->memoryUsage();
    }
    // 优化;防爆
    if (bodySize + pending.header.size() > MB(4))
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
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1000);

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            // 就处理读出和写入
            if (fd == event_fd)
            {
                uint64_t cnt;
                while (read(event_fd, &cnt, sizeof(cnt)) > 0)
                    ;
                // 清空计数
                while (handleTaskResultOnce())
                    ;
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

        // currentTick++;
    }
}

void SubReactor::addFd(int fd)
{
    // 设置为非堵塞
    fd_unblock(fd);
    epoll_event ev{};
    // 将epoll状态修改为监听该文件描述符读事件，使用边缘触发，且每次事件只会触发一次，处理完毕需手动重置监听
    // ONESHOT防止多个线程同时处理同一个 fd（你未来多 reactor 必用）,主要做作用就是通过使得重复fd多次发出通知
    // 避免重复触发（减少惊群）
    // 控制状态机
    // 优化：始终让epoll中的fd处于epollin和epollout
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = fd;

    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    // 初始化conns对象
    Connection conn;
    conn.fd = fd;
    conn.id = ++global_conn_id;
    conn.keepAlive = false;
    conn.state.readPaused = false;
    conns[fd] = conn;

    // 设置对应时间轮
    Conn tmpconn;
    tmpconn.fd = fd;
    wheel.add(tmpconn);
}
