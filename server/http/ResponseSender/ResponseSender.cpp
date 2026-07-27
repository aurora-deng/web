#include "ResponseSender.h"

SendState ResponseSender::send(int fd, HttpResponse &resp)
{
    // 响应头必须完整发送后才进入正文；各 Body 的消费偏移留在响应对象中，
    // SEND_AGAIN 返回时不会重建已发送部分，从而保证非阻塞续写不重复字节。
    auto s = sendHeader(fd, resp);

    if (s != SEND_OK)
        return s;

    if (!resp.body)
        return SEND_OK;

    if (resp.body->useSendfile())
        return sendFileBody(fd, resp);

    return sendMemoryBody(fd, resp);
}

SendState ResponseSender::sendHeader(int fd, HttpResponse &resp)
{
    while (resp.HeaderBody_->remain() > 0)
    {

        // 构建iov
        Block *block = segPool.acquire();
        // 由于后面存在判断会导致直接返回，所以创建变量BlockGuard随着函数的消失而自动析构释放block
        BlockGuard guard{segPool, block};

        // buildSegments 失败可能表示没有可发送段，也可能表示暂时等待生产者；
        // finished 用于区分“发送完成”和“稍后重试”。
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
            if (resp.HeaderBody_->remain() == 0)
                return SEND_OK;

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

        return SEND_CLOSED;
    }
    return SEND_OK;
}

SendState ResponseSender::sendMemoryBody(int fd, HttpResponse &resp)
{
    // body
    while (true)
    {
        // chunkd检查
        if (!resp.body)
        {
            return SEND_OK;
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

            // 只按内核实际接受的字节推进 Body；部分写是正常情况，不能假设 writev 一次发送全部段。
            wheel.refresh(fd);
            resp.body->consume(static_cast<size_t>(n));
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
                return SEND_CLOSED;
            }
        }
        else if (n == 0)
        {
            // 表示连接异常直接关闭即可
            LOG_ERROR(std::string("connect error: ") + strerror(errno));
            return SEND_CLOSED;
        }
    }
}

SendState ResponseSender::sendFileBody(int fd, HttpResponse &resp)
{
    // 判断是否为发送文件
    // auto file = std::dynamic_pointer_cast<FileBody>(resp.body);

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

    return SEND_CLOSED;
}
