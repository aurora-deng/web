#include "SubReactor.h"
#include <fcntl.h>
#include <atomic>
#include "server/http/http.h"
std::atomic<uint64_t> global_conn_id{0}; // 自增
// 统一套接字关闭
// 优化：防止其他的线程来杀死我当前线程的fd
void SubReactor::fd_close(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;

    // 优化：加上状态检查，防止当某fd已经关闭之后重复关闭或者关闭之后任然在fd
    if (it->second.state == CLOSED)
        return;

    it->second.state = CLOSED;
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
    if (it->second.state == CLOSED)
        return;
    epoll_event ev{};
    ev.events = EPOLLONESHOT | EPOLLET | events;
    ev.data.fd = fd;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == -1)
    {
        perror("epol_ctl MOD failed");
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
// 用于判断消息是否发送完
// 优化：同时要判断输出的数据是否完全输出
bool SubReactor::is_complete(const std::string &buf)
{
    size_t header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos)
        return false;

    size_t body_start = header_end + 4;

    // 默认没有body
    int content_length = 0;

    // 查找到Content-Length
    size_t pos = buf.find("Content-Length:");

    if (pos != std::string::npos)
    {
        size_t line_end = buf.find("\r\n", pos);

        std::string len_str = buf.substr(pos + 15, line_end - (pos + 15));

        content_length = std::stoi(len_str);
    }
    return buf.size() >= body_start + content_length;
}
// 写入函数
void SubReactor::handleWrite(int fd)
{
    // 优化，---加一层检查防止对于发到不存在直接创建出来一个新的
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    auto &conn = it->second;

    auto iter = conn.pendingResponses.find(conn.nextResponseSeq);
    if (iter == conn.pendingResponses.end()) // 表示当前该fd没有可以发送respons,切换为监听状态
    {
        rearm(fd, EPOLLIN);
        return;
    }
    // 使用循环防止要发送的消息大小大于socket的内核大小，导致后续的没发出去,保证这条消息完整的发出
    while (conn.pendingResponses.count(conn.nextResponseSeq))
    {
        // 按照顺序取出消息并回复
        auto &resp = conn.pendingResponses[conn.nextResponseSeq];
        if(resp.data.size()==resp.offset)break;
        int n = send(fd, resp.data.data() + resp.offset, resp.data.size() - resp.offset, 0);

        if (n > 0) // 清空已经发送的部分
        {
            resp.offset += n;
        }
        else if (n == -1) // 表示没有消息或者发送的消息发布完了
        {
            perror("send error");
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 发不完等下一次
                break;
            }
            else if (errno == EINTR)
            {
                // 信号被打断重新尝试
                continue;
            }
            else
            {
                // 出现错误
                fd_close(fd);
                return;
            }
        }
        else if (n == 0)
        {
            // 表示连接异常直接关闭即可
            fd_close(fd);
            return;
        }
    }
    // 将这一部分提出循环之外，放在循环内会重复调用浪费时间
    if (conn.pendingResponses[conn.nextResponseSeq].data.size() == conn.pendingResponses[conn.nextResponseSeq].offset)
    {
        conn.pendingResponses.erase(conn.nextResponseSeq);
        conn.nextResponseSeq++;
        // 优化防止直接结束fd之后又要重新连接，直接更改模式
        conn.state = ConnState::READING;

        if (conn.keepAlive)
        {
            conn.state = READING;
            if (!conn.pendingRequests.empty())
            {

                while (!conn.pendingRequests.empty())
                    if(!processRequest(fd))break;

                rearm(fd,EPOLLIN);
            }
            else
                // 重新激活为监听状态
                rearm(fd, EPOLLIN);
        }
        else
        {
            fd_close(fd);
        }
    }
    else
    {
        conn.state = WRITING;
        rearm(fd, EPOLLOUT|EPOLLIN);
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
        conn.readPaused = true;
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
                fd_close(fd);
                closed = true; // 更新close用于后面重新唤醒
                break;
            }
        }
        else if (res == 0)
        {
            // printf("对端数据已经下线\n");
            fd_close(fd);
            closed = true;
            break;
        }
        // printf("收到数据：%.*s\n", res, buffer);

        // 开始处理数据
        conn.readBuffer.append(buffer, res);
        conn.pendingBytes = conn.readBuffer.size();
        // 可优化点：使用零拷贝  std::string localBuf.swap(conns[fd].readBuffer);
        //  或者使用现在的多reactor直接对conns进行操作
    }

    if (closed)
        return;

    // 循环防止由于系统内核中相对于所要发送的消息而言内存不够，所以需要使用循环处理httpRequest,防止粘包
    // 优化去掉循环，防止发到被多次调用
    // 优化减轻readBUffer的负担，同时循环处理请求将其塞入到对应的请求队列中
    while (is_complete(conn.readBuffer))
    {
        PendingRequest req;
        req.data = extract_request(conn.readBuffer);
        req.seq = conn.nextRequestSeq++;
        conn.pendingRequests.push(req);
    }
    if (!conn.pendingRequests.empty())
    {
        while (!conn.pendingRequests.empty())
            if(!processRequest(fd)) break;

        rearm(fd,EPOLLIN);
    }
    else
    {
        rearm(fd, EPOLLIN);
    }

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

std::string SubReactor::extract_request(std::string &readBuffer)
{
    // 解析http请求
    // 找到对应的一个完整请求的位置，不过只处理post没有处理其他的请求方式，后续需要完善httpRequest、
    size_t pos = readBuffer.find("\r\n\r\n");

    size_t body_start = pos + 4;

    int content_length = 0;
    pos = readBuffer.find("Content-Length:");
    if (pos != std::string::npos)
    {
        size_t line_end = readBuffer.find("\r\n", pos);

        std::string len_str = readBuffer.substr(pos + 15, line_end - (pos + 15));

        content_length = std::stoi(len_str);
    }
    size_t total_len = body_start + content_length;

    std::string request_data;
    if (total_len > readBuffer.size())
    {
        request_data = readBuffer;
        readBuffer.clear();
    }
    else
    {
        request_data = readBuffer.substr(0, total_len);
        readBuffer.erase(0, total_len);
    }

    return request_data;
}
bool SubReactor::processRequest(int fd)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return false;
    auto &conn = it->second;
    if (it->second.state != READING)
        return false;
    // 优化:防止队列数据过载
    if (conn.inflightTasks >= MAX_PIPELINE)
    {
        return false;
    }
    conn.inflightTasks++;
    // 将处理好的消息取出
    PendingRequest request = conn.pendingRequests.front();
    conn.pendingRequests.pop();
    conn.pendingBytes = conn.readBuffer.size();
    if (conn.pendingBytes < MAX_PENDING_BYTES)
        conn.readPaused = false;

    uint64_t cid = conn.id;
    // conn.state = PROCESSING;
    SubReactor *reactor = this;

    pool.addTask([fd, cid, request, reactor]
                 {
                             TaskResult result;
                             result.fd = fd;
                             result.id = cid;
                             result.seq=request.seq;
                            //  result.state=PROCESSING;
                             // 解析Http
                             HttpRequest req = parse_request(request.data);

                             auto it = req.headers.find("Connection");

                             // 优化：仔细处理Http/1.0，防止误判keep-alive,防止http1.0直接误判keep-alive
                             if (req.version == "HTTP/1.1")
                             {
                                 // 默认为keep-alive
                                 if (it != req.headers.end() && it->second == "close")
                                 {
                                     result.keepAlive = false;
                                 }
                                 else
                                     result.keepAlive = true;
                             }
                             else
                             { // HTTP/1.0
                                 // 默认close
                                 if (it != req.headers.end() && it->second == "keep-alive")
                                 {
                                     result.keepAlive = true;
                                 }
                                 else
                                 {
                                     result.keepAlive = false;
                                 }
                             }

                             // 优化：使用keep-alive来标记连接状态，必须按照协议来使用body length来保障对响应时间，严格遵循协议
                             std::string body = "<h1>Hello Epoll" + req.path + "</h1>";

                             std::string response = // HTTP/1.1 默认 keep-alive
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/html\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) + "\r\n";

                             if (result.keepAlive)
                             {
                                 response += "Connection: keep-alive\r\n";
                             }
                             else
                             {
                                 response += "Connection: close\r\n";
                             }

                             response += "\r\n" + body;
                             result.response = response;

                             reactor->pushResult(result); });
    return true;
}

void SubReactor::pushResult(const TaskResult &res)
{
    bool needWalk = false;
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        if (push_to_SubReactor_queue.empty())
        {
            needWalk = true;
        }
        push_to_SubReactor_queue.push(res);
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
                perror("eventfd write");
            }
        }
    }
}
bool SubReactor::handleTaskResultOnce()
{
    // 单次畜类每次的worker的返回的结果
    // 等每轮消息处理完之后将信息取出来，减少对锁的持有和竞争
    TaskResult task;
    // 使用局部作用域，让锁尽快释放
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        if (push_to_SubReactor_queue.empty())
            return false;

        task = push_to_SubReactor_queue.front();
        push_to_SubReactor_queue.pop();
    }
    // 优化：同时也是使用find查找，防止高并发导致fd误杀
    auto it = conns.find(task.fd);
    if (it == conns.end())
        return true;
    if (it->second.id != task.id)
        return true;
    it->second.inflightTasks--;
    // bool needEnableWrite = it->second.writeBuffer.empty();
    // 将信息拆分之后返回个conns并更新conns的状态
    it->second.state = ConnState::WRITING;
    it->second.keepAlive = task.keepAlive;
    it->second.pendingResponses[task.seq].data = task.response;
    // 如果之前没有需要写的，则需要重新唤醒对应fd为epollout状态
    // if (needEnableWrite)
    rearm(task.fd, EPOLLOUT|EPOLLIN);
    // 优化;防爆
    if (it->second.pendingResponses[task.seq].data.size() > 1024 * 1024)
    {
        fd_close(task.fd);
        return false;
    }

    return true;
}
void SubReactor::loop()
{
    // 创建epoll储存大小
    epoll_event events[MAX_EVENTS];

    while (true)
    {
        // 开始监听epfd并将数据存放到events中
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            // 就处理读出和写入
            if (fd == event_fd)
            {
                uint64_t cnt;
                while (read(event_fd, &cnt, sizeof(cnt)) > 0)
                    ; // 清空计数
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
    conn.state = ConnState::READING;
    conns[fd] = conn;
}