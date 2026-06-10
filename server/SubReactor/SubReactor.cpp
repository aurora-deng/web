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
void SubReactor::fd_close(int fd,std::string reason)
{
    auto it = conns.find(fd);
    if (it == conns.end())
        return;
    std::cout
        << "[CLOSE]"
        << " fd="
        << fd
        << " reason="
        << reason
        << " "
        << strerror(errno)
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
// 使用状态机来处理返回结果
enum SendState
{
    SEND_OK,
    SEND_AGAIN,
    SEND_CLOSED
};

// 正常发送
static SendState sendWritev(int fd, pendingResponse &resp)
{
    int NeedSentBytes = (resp.header.size() - resp.headerOffset) + (resp.body->data.size() - resp.bodyOffset);
    iovec vec[2];
    while (NeedSentBytes > 0)
    {
        int iovcnt = 0;
        if (resp.headerOffset < resp.header.size())
        {
            vec[iovcnt].iov_base = (void *)(resp.header.data() + resp.headerOffset);
            vec[iovcnt++].iov_len = resp.header.size() - resp.headerOffset;
        }

        if (resp.bodyOffset < resp.body->data.size())
        {
            vec[iovcnt].iov_base = (void *)(resp.body->data.data() + resp.bodyOffset);
            vec[iovcnt++].iov_len = resp.body->data.size() - resp.bodyOffset;
        }
        int n = writev(fd, vec, iovcnt);
        if (n > 0)
        {
            NeedSentBytes -= n;
            if (resp.headerOffset + n > resp.header.size())
            {

                n -= (resp.header.size() - resp.headerOffset);
                resp.headerOffset = resp.header.size();
                resp.bodyOffset += n;
            }
            else
            {
                resp.headerOffset += n;
            }
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
                return SEND_CLOSED;
            }
        }
        else if (n == 0)
        {
            // 表示连接异常直接关闭即可
            return SEND_CLOSED;
        }
    }

    return SEND_OK;
}

// 发送chunked处理
static SendState sendChunk(int fd, ChunkBolck &chunk)
{

    int total = chunk.prefix.size() + chunk.data->data.size() + chunk.suffix.size();
    int NeedSent = total - chunk.sent;
    iovec vec[4];
    while (NeedSent > 0)
    {
        int iovcnt = 0;
        if (chunk.sent < chunk.prefix.size())
        {
            vec[iovcnt].iov_base = (void *)(chunk.prefix.data() + chunk.sent);
            vec[iovcnt++].iov_len = chunk.prefix.size() - chunk.sent;
        }

        if (chunk.sent < chunk.prefix.size() + chunk.data->data.size())
        {
            size_t dataOffset = 0;

            if (chunk.sent > chunk.prefix.size())
            {
                dataOffset =
                    chunk.sent - chunk.prefix.size();
            }
            vec[iovcnt].iov_base = (void *)(chunk.data->data.data() + dataOffset);
            vec[iovcnt++].iov_len = chunk.data->data.size() - dataOffset;
        }

        if (chunk.sent < chunk.prefix.size() + chunk.data->data.size() + chunk.suffix.size())
        {
            size_t suffixOffset = 0;

            if (chunk.sent > chunk.prefix.size() + chunk.data->data.size())
            {
                suffixOffset =
                    chunk.sent - chunk.prefix.size() - chunk.data->data.size();
            }
            vec[iovcnt].iov_base = (void *)(chunk.suffix.data() + suffixOffset);
            vec[iovcnt++].iov_len = chunk.suffix.size() - suffixOffset;
        }
        int n = writev(fd, vec, iovcnt);
        if (n > 0)
        {
            chunk.sent += n;
            NeedSent -= n;
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
                return SEND_CLOSED;
            }
        }
        else if (n == 0)
        {
            // 表示连接异常直接关闭即可
            return SEND_CLOSED;
        }
    }

    return SEND_OK;
}
// 头文件发送函数
static SendState sendHeader(int fd, pendingResponse &resp)
{
    
    // 先发送头文件
    while (resp.headerOffset < resp.header.size())
    {
        int n = send(fd, resp.header.data() + resp.headerOffset, resp.header.size() - resp.headerOffset, MSG_NOSIGNAL);
        if (n > 0) // 清空已经发送的部分
        {
            resp.headerOffset += n;
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
                return SEND_CLOSED;
            }
        }
        else if (n == 0)
        {
            // 表示连接异常直接关闭即可
            return SEND_CLOSED;
        }
    }
    bool headerDone = (resp.headerOffset == resp.header.size());
    // 判断头文件是否发完，如果没发完，直接进入下一次循环，之后二次发送
    if (!headerDone)
    {
        return SEND_AGAIN;
    }
    return SEND_OK;
}

// 静态文件发送函数
static SendState sendEndChunk(int fd, pendingResponse &resp)
{
    // 发送一个最后的endChunk

    while (resp.EndchunkOffset < resp.endChunks.size())
    {
        int n = send(fd, resp.endChunks.data() + resp.EndchunkOffset, resp.endChunks.size() - resp.EndchunkOffset, MSG_NOSIGNAL);
        if (n > 0) // 清空已经发送的部分
        {
            resp.EndchunkOffset += n;
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
                return SEND_CLOSED;
            }
        }
        else if (n == 0)
        {
            // 表示连接异常直接关闭即可
            return SEND_CLOSED;
        }
    }
    return SEND_OK;
}

static SendState sendFile(int fd, pendingResponse &resp)
{
    while (resp.filebody.remain > 0)
    {
        int n = sendfile(fd, resp.filebody.fd, &resp.filebody.offset, resp.filebody.remain);
        if (n > 0)
        {
            resp.filebody.remain -= n;
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
                return SEND_CLOSED;
            }
        }
        else if (n == 0)
        {
            // 表示连接异常直接关闭即可
            return SEND_CLOSED;
        }
    }
    return SEND_OK;
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


        // ——————————————————————————————————————————————————————————————————日志
        if (!resp.useSendfile &&
            !resp.chunked &&
            !resp.body)
        {
            // std::cout
            //     << "RESP BODY NULL "
            //     << fd
            //     << std::endl;

            fd_close(fd,"RESP BODY NULL ");
            return;
        }
        // 使用循环防止要发送的消息大小大于socket的内核大小，导致后续的没发出去,保证这条消息完整的发出
        // // 先发送头文件
        // while (resp.headerOffset < resp.header.size())
        // {
        //     int n = send(fd, resp.header.data() + resp.headerOffset, resp.header.size() - resp.headerOffset, MSG_NOSIGNAL);
        //     if (n > 0) // 清空已经发送的部分
        //     {
        //         resp.headerOffset += n;
        //     }
        //     else if (n == -1) // 表示没有消息或者发送的消息发布完了
        //     {
        //         LOG_ERROR(std::string("send error: ") + strerror(errno));
        //         if (errno == EAGAIN || errno == EWOULDBLOCK)
        //         {
        //             // 发不完等下一次
        //             break;
        //         }
        //         else if (errno == EINTR)
        //         {
        //             // 信号被打断重新尝试
        //             continue;
        //         }
        //         else
        //         {
        //             // 出现错误
        //             fd_close(fd);
        //             return;
        //         }
        //     }
        //     else if (n == 0)
        //     {
        //         // 表示连接异常直接关闭即可
        //         fd_close(fd);
        //         return;
        //     }
        // }

        // // 判断发送chunked还是常规body
        // if (!resp.chunked)
        // {
        //     while (resp.bodyOffset < resp.body->data.size())
        //     {
        //         int n = send(fd, resp.body->data.data() + resp.bodyOffset, resp.body->data.size() - resp.bodyOffset, MSG_NOSIGNAL);
        //         if (n > 0) // 清空已经发送的部分
        //         {
        //             resp.bodyOffset += n;
        //         }
        //         else if (n == -1) // 表示没有消息或者发送的消息发布完了
        //         {
        //             LOG_ERROR(std::string("send error: ") + strerror(errno));
        //             if (errno == EAGAIN || errno == EWOULDBLOCK)
        //             {
        //                 // 发不完等下一次
        //                 break;
        //             }
        //             else if (errno == EINTR)
        //             {
        //                 // 信号被打断重新尝试
        //                 continue;
        //             }
        //             else
        //             {
        //                 // 出现错误
        //                 fd_close(fd);
        //                 return;
        //             }
        //         }
        //         else if (n == 0)
        //         {
        //             // 表示连接异常直接关闭即可
        //             fd_close(fd);
        //             return;
        //         }
        //     }
        // }else{

        // }

        if (resp.useSendfile)
        {

            // 先发送头文件
            switch (sendHeader(fd, resp))
            {
            case SEND_OK:
                break;
            case SEND_AGAIN:
                break;
            case SEND_CLOSED:
                // std::cout
                //     << "sendHeader close\n";
                fd_close(fd,"sendHeader close");
                return;
            }

            switch (sendFile(fd, resp))
            {
            case SEND_OK:
                break;
            case SEND_AGAIN:
                break;
            case SEND_CLOSED:
                // std::cout
                //     << "sendFile close\n";
                // fd_close(fd,"sendFile close");
                // 关闭文件,优化统一由filecache关闭
                // close(resp.filebody.fd);
                return;
            }
            if (resp.filebody.remain == 0 && resp.header.size() == resp.headerOffset)
            {
                if(resp.useSendfile)
                {
                    FileCache::instace().put(resp.filebody.filepath);
                }
                conn.pendingResponses.erase(conn.nextResponseSeq);
                conn.nextResponseSeq++;
            }
            else
            {
                break;
            }
        }
        else
        {
            // 优化，使用抽象函数处理
            if (!resp.chunked)
            {
                switch (sendWritev(fd, resp))
                {
                case SEND_OK:
                    break;
                case SEND_AGAIN:
                    break;
                case SEND_CLOSED:
                    // std::cout
                    //     << "sendwritev close\n";
                    // fd_close(fd,"sendwritev close");
                    return;
                }
                if (resp.body->data.size() == resp.bodyOffset && resp.header.size() == resp.headerOffset)
                {
                    conn.pendingResponses.erase(conn.nextResponseSeq);
                    conn.nextResponseSeq++;
                }
                else
                {
                    break;
                }
            }
            else
            {
                // 先发送头文件
                switch (sendHeader(fd, resp))
                {
                case SEND_OK:
                    break;
                case SEND_AGAIN:
                    break;;
                case SEND_CLOSED:
                    // std::cout
                    //     << "sendHeader close\n";
                    fd_close(fd,"sendHeader close");
                    return;
                }

                // 开始发送chunk，使用流式
                while (!resp.stream->chunks.empty())
                {
                    auto &chunk = resp.stream->chunks.front();

                    SendState st = sendChunk(fd, chunk);

                    if (st == SEND_OK)
                    {
                        size_t total = chunk.prefix.size() + chunk.data->data.size() + chunk.suffix.size();
                        if (chunk.sent == total)
                        {
                            resp.stream->chunks.pop_front();
                        };
                        continue;
                    }
                    else if (st == SEND_AGAIN)
                    {
                        break;
                    }
                    else
                    {

                        fd_close(fd,"sendChunk error");
                        return;
                    }
                }

                if (resp.stream->chunks.empty() && resp.stream->finished)
                {
                    switch (sendEndChunk(fd, resp))
                    {
                    case SEND_OK:
                        break;
                    case SEND_AGAIN:
                        break;
                    case SEND_CLOSED:
                        // std::cout
                        //     << "sendChunk close\n";
                        fd_close(fd,"sendChunk close");
                        return;
                    }
                    // 一个stram彻底生命周期结束的地方
                    if (resp.stream && resp.chunked)
                    {
                        conn.inflightTasks--;
                        if (conn.inflightTasks < MAX_PIPELINE)
                        {
                            conn.state.pauseByPipeline = false;
                            updateEvent(fd);
                        }
                    }
                    conn.pendingResponses.erase(conn.nextResponseSeq);
                    conn.nextResponseSeq++;
                }
                else
                {
                    break;
                }
            }
        }

        // 更新时间戳

        conn.lastActive = std::chrono::steady_clock::now();
    }

    // 将这一部分提出循环之外，放在循环内会重复调用浪费时间
    if (conn.pendingResponses.empty())
    {

        // 优化防止直接结束fd之后又要重新连接，直接更改模式
        conn.state.wantWrite = false;
        if (conn.keepAlive)
        {
            if (!conn.pendingRequests.empty())
            {

                while (!conn.pendingRequests.empty())
                    if (!processRequest(fd))
                        break;

                if (!conn.pendingResponses.empty())
                    conn.state.wantWrite = true;
                updateEvent(fd);
            }
            else
            {
                // 重新激活为监听状态
                updateEvent(fd);
            }
        }
        else
        {
            // printf("conn.keepAlive false\n");
            fd_close(fd,"conn.keepAlive false");
        }
    }
    else
    {
        conn.state.wantWrite = true;
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
                fd_close(fd,"连接失败");
                closed = true; // 更新close用于后面重新唤醒
                break;
            }
        }
        else if (res == 0)
        {
            // printf("对端数据已经下线\n");
            fd_close(fd,"对端数据已经下线");
            closed = true;
            break;
        }
        // printf("收到数据：%.*s\n", res, buffer);

        // 开始处理数据
        conn.readBuffer.append(buffer, res);
        conn.pendingBytes = conn.readBuffer.buf.size();
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
            fd_close(fd,"try_parse_request PARSE_ERROR");
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
    conn.lastActive = std::chrono::steady_clock::now();

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

                             // 优化：使用keep-alive来标记连接状态，必须按照协议来使用body length来保障对响应时间，严格遵循协议
                            //  优化:使用router
                            //  std::string body = "<h1>Hello Epoll" + req.path + "</h1>";

                            //  std::string response = // HTTP/1.1 默认 keep-alive
                            //      "HTTP/1.1 200 OK\r\n"
                            //      "Content-Type: text/html\r\n"
                            //      "Content-Length: " +
                            //      std::to_string(body.size()) + "\r\n";

                            //  if (result.keepAlive)
                            //  {
                            //      response += "Connection: keep-alive\r\n";
                            //  }
                            //  else
                            //  {
                            //      response += "Connection: close\r\n";
                            //  }

                            //  response += "\r\n" + body;

                            
                             result.header=resp.buildHeader();
                             result.body=resp.body;
                             result.chunked=resp.chunked;
                             result.stream=resp.stream;
                             result.fileFd=resp.filefd;
                             result.useSendfile=resp.useSendfile;
                             result.fileSize=resp.fileSize;
                             result.sendBegin=resp.sendBegin;
                             result.sendEnd=resp.sendEnd;
                             result.filepath=resp.filePath;

                             result.keepAlive=resp.keepAlive;
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
        if (StreamNotifyQueue.empty())
        {
            needWalk = true;
        }
        StreamNotifyQueue.push({fd, id});
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

    TaskResult task;
    // 使用局部作用域，让锁尽快释放
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        if (push_to_SubReactor_queue.empty())
            return false;

        task = push_to_SubReactor_queue.front();
        push_to_SubReactor_queue.pop();
    }

    // __________________________________________________________________日志检查
    if (!task.body)
    {
        std::cout
            << "BODY NULL fd="
            << task.fd
            << " seq="
            << task.seq
            << std::endl;
    }

    // 优化：同时也是使用find查找，防止高并发导致fd误杀
    auto it = conns.find(task.fd);
    if (it == conns.end())
        return true;
    if (it->second.id != task.id)
        return true;

    // 非流式发送（chunk）的生命周期结束点
    if (!task.chunked)
        it->second.inflightTasks--;

    bool needRearmRead = false;
    if (it->second.inflightTasks < MAX_PIPELINE && it->second.state.pauseByPipeline)
    {
        it->second.state.pauseByPipeline = false;
        needRearmRead = true;
    }

    // bool needEnableWrite = it->second.writeBuffer.empty();
    // 将信息拆分之后返回个conns并更新conns的状态
    it->second.state.wantWrite = true;
    it->second.keepAlive = task.keepAlive;

    // it->second.pendingResponses[task.seq].data = std::move(task.response);
    // 使用零拷贝优化
    auto &pending = it->second.pendingResponses[task.seq];

    pending.header = std::move(task.header);
    // 下面三种都是用了共享指针实现零拷贝优化
    pending.body = task.body;
    pending.chunked = task.chunked;
    pending.stream = task.stream;
    pending.useSendfile = task.useSendfile;
    pending.filebody.fd = task.fileFd;
    pending.filebody.size = task.fileSize;
    pending.filebody.filepath=task.filepath;
    pending.filebody.offset = task.sendBegin;
    pending.filebody.remain = task.sendEnd - task.sendBegin + 1;
    // 使用回调函数自己唤醒
    if (pending.stream)
    {
        pending.stream->wakeup = [reactor = this, fd = task.fd, cid = task.id]
        {
            reactor->notifyStream(fd, cid);
        };
    }
    // 如果之前没有需要写的，则需要重新唤醒对应fd为epollout状态
    if (needRearmRead || it->second.pendingResponses.size() == 1)
    {
        it->second.state.wantWrite = true;
        updateEvent(task.fd);
    }
    // 防空指针
    size_t bodySize = 0;
    if (pending.body)
    {
        bodySize = pending.body->data.size();
    }
    // 优化;防爆
    if (bodySize + pending.header.size() > 1024 * 1024)
    {
        // std::string waning="爆了";
        // LOG_INFO(waning+strerror(errno));
        fd_close(task.fd,"bodySize爆了");
        return false;
    }
    // std::string wanning = "fd=" + std::to_string(task.fd);
    // wanning += "inflight=" + std::to_string(it->second.inflightTasks);
    // wanning += "pending=" + std::to_string(it->second.pendingResponses.size());
    // LOG_INFO(
    //     wanning + strerror(errno));

    return true;
}
// 作用：有新的chunk来了就唤醒epollout
bool SubReactor::handleStreamNotify()
{
    // 模仿handleTaskResultOnce处理stram的流式唤醒
    StreamNotify task;
    // 使用局部作用域，让锁尽快释放
    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        if (StreamNotifyQueue.empty())
            return false;

        task = StreamNotifyQueue.front();
        StreamNotifyQueue.pop();
    }
    // 优化：同时也是使用find查找，防止高并发导致fd误杀
    auto it = conns.find(task.fd);
    if (it == conns.end())
        return true;
    if (it->second.id != task.connId)
        return true;
    it->second.state.wantWrite = true;

    updateEvent(task.fd);

    return true;
}
void SubReactor::loop()
{
    // 创建epoll储存大小
    epoll_event events[MAX_EVENTS];
    auto last=std::chrono::steady_clock::now();
    while (true)
    {
        // 开始监听epfd并将数据存放到events中,优化100ms无连接超时
        int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
        auto now=std::chrono::steady_clock::now();

        auto sec=std::chrono::duration_cast<std::chrono::seconds>(now-last).count();        //刷新计时
        if(sec>=1)
        {
            wheel.tick();
            last=now;
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
                // 清空计数
                while (handleTaskResultOnce())
                    ;
                while (handleStreamNotify())
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

        currentTick++;
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
    wheel.add(fd,30);
}
