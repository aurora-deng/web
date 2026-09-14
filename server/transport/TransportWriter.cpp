// =============================================================================
// 文件名：TransportWriter.cpp
// 所属模块：server/transport —— socket 写入口的实现
//
// 【职责比喻：发货员的"工作手册"】
//   本文件实现 TransportWriter 的入队背压判定（enqueue）、排空循环（flush）、字节串聚集写
//   （flushEncoded）、Http 响应混合写（flushHttp）、弹单回填票据（completeFront）、待写计数
//   扣减与水位恢复（consumeBufferedBytes）。匿名命名空间里还藏着内存体分段写（writeMemoryBody）
//   与 Http 响应整体写（writeHttpResponse）两个工具函数。所有 writev / sendfile 系统调用
//   都收口在这里。
//
// 关键技术点（初学者重点理解）：
//   1. 【EAGAIN / EINTR 处理】writev 部分写返回 EAGAIN 时返回 Blocked 等下次 EPOLLOUT；
//      EINTR 被信号打断则重试。EAGAIN 不算错误，是"写满了"的正常背压信号。
//   2. 【writev 聚集 + 公平预算】flushEncoded 把队列前若干张单拼进 iovec[64]，单次系统
//      调用最多 64KiB，整轮 flush 最多实际写 256KiB，避免单连接独占 Reactor。
//   3. 【sendfile 零拷贝】文件型 body 走 sendFile，绕开用户态缓冲；返回 -2 表示阻塞（EAGAIN）。
//   4. 【水位闭环】enqueue 超高水位置 pauseByWrite 暂停读；consumeBufferedBytes / flush
//      排空后跌回低水位则撤销 pauseByWrite 恢复读，形成完整背压闭环。
//   5. 【ticket 单调回填】completeFront 只增不降地更新 completedTicket，保证等待协程能正确
//      被唤醒；CloseConnection 单写完置 closeRequested 让上层关连接。
// =============================================================================
#include "TransportWriter.h"

#include "log/logger/logger.h"
#include "server/SegmentPool/SegmentPool.h"
#include "server/http/http.h"
#include "server/timer/TimeWheel.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/uio.h>
#include <unistd.h>

namespace {

// ---- 匿名命名空间：本编译单元私有的写辅助 ----

// 字节串载荷的"游标"：指向数据起点 + 总长 + 指向 offset 的指针（offset 在 task 内，写后回填）
struct EncodedCursor
{
    const char *data = nullptr;
    std::size_t size = 0;
    std::size_t *offset = nullptr;
};

// 从 task 取游标：独占串与共享串两种 encoded 载荷都返回 {data, size, &offset}；否则空游标
EncodedCursor encodedCursor(OutboundTask &task)
{
    if (auto *owned = std::get_if<EncodedBufferTask>(&task.payload))
        return {owned->bytes.data(), owned->bytes.size(), &owned->offset};
    if (auto *shared = std::get_if<SharedEncodedBufferTask>(&task.payload);
        shared && shared->bytes)
        return {
            shared->bytes->data(), shared->bytes->size(), &shared->offset};
    return {};
}

// 判断是否 encoded 类载荷（独占或共享字节串），用于 flush 分流到 flushEncoded
bool isEncodedTask(const OutboundTask &task)
{
    return std::holds_alternative<EncodedBufferTask>(task.payload) ||
           std::holds_alternative<SharedEncodedBufferTask>(task.payload);
}

// Http 体写入结果：Done 写完 / Blocked 写到 EAGAIN / Error 出错
enum class HttpWriteResult
{
    Done,
    Blocked,
    Yielded,
    Error
};

// 内存体分段写：循环用对象池 Block 拼 iovec 再 writev，直到 finished 或阻塞
HttpWriteResult writeMemoryBody(int fd,
                                RespBody &body,
                                SegmentPool &pool,
                                TimerWheel &wheel,
                                std::size_t &byteBudget)
{
    while (!body.finished())
    {
        if (byteBudget == 0)
            return HttpWriteResult::Yielded;
        Block *block = pool.acquire();            // 从对象池借一个 Block
        BlockGuard guard{pool, block};            // RAII：出作用域自动归还 Block
        const std::size_t offer =
            std::min<std::size_t>(65536, byteBudget);
        if (!body.buildSegments(block, offer))    // 把体分段填进 Block（上限 64KiB）
            return body.finished() ? HttpWriteResult::Done
                                   : HttpWriteResult::Blocked;

        iovec vec[64];
        const int count = BlockToIov(block, vec, 64);  // Block → iovec 数组
        const ssize_t n = ::writev(fd, vec, count);     // 聚集写
        if (n > 0)
        {
            const auto written = static_cast<std::size_t>(n);
            body.consume(written);                      // 按已写字节推进体游标
            byteBudget -= written;
            wheel.refresh(fd);                          // 写成功刷新连接活跃度，防超时误杀
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;                                   // 被信号打断，重试
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return HttpWriteResult::Blocked;            // 写满，等下次 EPOLLOUT
        return HttpWriteResult::Error;                  // 其他错误
    }
    return HttpWriteResult::Done;
}

// Http 响应整体写：先写 HeaderBody，再写 body（文件走 sendfile，否则走 writeMemoryBody）
HttpWriteResult writeHttpResponse(int fd,
                                  HttpResponse &response,
                                  SegmentPool &pool,
                                  TimerWheel &wheel,
                                  std::size_t &byteBudget)
{
    // ---- 先把响应头分段写完 ----
    if (response.HeaderBody_ && !response.HeaderBody_->finished())
    {
        const auto header = writeMemoryBody(
            fd, *response.HeaderBody_, pool, wheel, byteBudget);
        if (header != HttpWriteResult::Done)
            return header;                              // 头没写完（阻塞/出错），直接返回
    }

    if (!response.body || response.body->finished())
        return HttpWriteResult::Done;                   // 没有体或体已写完，收工

    // ---- 体部走 sendfile 零拷贝（文件型 body）----
    if (response.body->useSendfile())
    {
        while (!response.body->finished())
        {
            if (byteBudget == 0)
                return HttpWriteResult::Yielded;
            const ssize_t n = response.body->sendFile(fd, byteBudget);
            if (n > 0)
            {
                byteBudget -= static_cast<std::size_t>(n);
                wheel.refresh(fd);                      // 写成功刷新活跃度
                continue;
            }
            if (n == -2)
                return HttpWriteResult::Blocked;        // -2 是本框架约定的 EAGAIN 信号
            return HttpWriteResult::Error;
        }
        return HttpWriteResult::Done;
    }

    // ---- 非文件体走内存分段 writev ----
    return writeMemoryBody(fd, *response.body, pool, wheel, byteBudget);
}

} // namespace

// ---- TransportWriter 公有方法 ----

// 入队 + 背压判定：连接已关直接丢；encoded 类超水位则暂停读返回 Backpressure
EnqueueResult TransportWriter::enqueue(Connection &conn, OutboundTask task)
{
    if (conn.state.closed)
    {
        task.complete(OutboundOutcome::Closed);
        return EnqueueResult::Closed;                   // 连接已死，丢弃
    }

    const bool encodedTask = isEncodedTask(task);
    const bool closeTask =
        task.completion == OutboundCompletion::CloseConnection;
    const std::size_t buffered = encodedTask ? task.remainingBytes() : 0;
    const bool tooManyTasks =
        conn.transport.outboundQueue.size() >= kMaxOutboundTasks;
    // 用减法判断，避免 pending + buffered 在 size_t 上溢后绕过水位。
    const bool tooManyBytes = encodedTask &&
        (buffered > kWriteHighWatermark ||
         conn.transport.pendingWriteBytes > kWriteHighWatermark - buffered);
    // CloseConnection 尾包必须有机会排到队尾；普通任务超过任一上限就拒绝。
    if (!closeTask && (tooManyTasks || tooManyBytes))
    {
        conn.transport.pauseByWrite = true;             // 标记写积压暂停
        conn.state.readPaused = true;                   // 撤销 EPOLLIN，停止读防雪崩
        task.complete(OutboundOutcome::Backpressured);
        return EnqueueResult::Backpressure;
    }

    conn.transport.pendingWriteBytes += buffered;       // 登记待写计数
    conn.transport.outboundQueue.push_back(std::move(task));  // 压队尾
    conn.state.wantWrite = true;                        // 标记有数据要写，驱动 writerLoop
    return EnqueueResult::Ok;
}

// 公平冲刷循环：逐张写队首，直到空 / 阻塞 / 预算耗尽 / 出错 / 请求关连接
FlushResult TransportWriter::flush(Connection &conn, std::size_t byteBudget)
{
    bool closeRequested = false;
    const std::size_t initialBudget = byteBudget;
    while (!conn.transport.outboundQueue.empty())
    {
        if (byteBudget == 0)
            return {
                FlushStatus::Yielded,
                closeRequested,
                initialBudget};

        FlushStatus status;
        if (isEncodedTask(conn.transport.outboundQueue.front()))
            status = flushEncoded(conn, closeRequested, byteBudget);
        else
            status = flushHttp(conn, closeRequested, byteBudget);

        if (status != FlushStatus::Drained || closeRequested)
            return {
                status,
                closeRequested,
                initialBudget - byteBudget};
    }

    // ---- 队列排空：撤销 wantWrite，并检查是否可恢复读 ----
    conn.state.wantWrite = false;
    if (conn.transport.pauseByWrite &&
        conn.transport.pendingWriteBytes <= kWriteLowWatermark)  // 跌回低水位
    {
        conn.transport.pauseByWrite = false;
        conn.state.readPaused = conn.state.pauseByMemory;       // 只保留内存背压，撤销写背压
    }
    return {
        FlushStatus::Drained,
        closeRequested,
        initialBudget - byteBudget};
}

// 字节串单聚集写：拼 iovec[64] 一次 writev 跨多张单
FlushStatus TransportWriter::flushEncoded(Connection &conn,
                                          bool &closeRequested,
                                          std::size_t &byteBudget)
{
    iovec vec[64];
    int count = 0;
    std::size_t offered = 0;
    // ---- 从队首往后扫，把各张单的剩余段拼进 iovec，上限 64 段 / 64KiB ----
    for (auto &task : conn.transport.outboundQueue)
    {
        auto cursor = encodedCursor(task);
        if (!cursor.offset || count == 64 ||
            offered >= 65536 || offered >= byteBudget)
            break;                                     // 非 encoded 或拼满，停止
        const std::size_t remain = cursor.size - *cursor.offset;
        if (remain == 0)
            break;                                     // 本单已写完（理论上下轮 completeFront 会弹掉）
        const std::size_t length = std::min(
            remain,
            std::min<std::size_t>(65536 - offered, byteBudget - offered));
        vec[count++] = {
            const_cast<char *>(cursor.data + *cursor.offset),
            length};
        offered += length;
        // CloseConnection 是发送屏障：它后面的迟到任务不能被同一次 writev 偷跑出去。
        if (task.completion == OutboundCompletion::CloseConnection)
            break;
    }

    if (count == 0)
    {
        completeFront(conn, closeRequested);           // 没东西可写（空单），直接弹掉
        return FlushStatus::Drained;
    }

    const ssize_t n = ::writev(conn.fd, vec, count);   // 一次聚集写
    if (n < 0 && errno == EINTR)
        return FlushStatus::Drained;                   // 被信号打断，下轮再来
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return FlushStatus::Blocked;                   // 写满，等 EPOLLOUT
    if (n <= 0)
    {
        LOG_ERROR(std::string("transport writev error: ") + std::strerror(errno));
        return FlushStatus::Error;
    }

    wheel_.refresh(conn.fd);                           // 写成功刷新活跃度
    std::size_t consumed = static_cast<std::size_t>(n);
    byteBudget -= consumed;
    consumeBufferedBytes(conn, consumed);              // 扣减待写计数
    // ---- 按已写字节逐张推进 offset，写完的弹掉 ----
    while (consumed > 0 && !conn.transport.outboundQueue.empty())
    {
        auto cursor = encodedCursor(conn.transport.outboundQueue.front());
        if (!cursor.offset)
            break;
        const std::size_t remain = cursor.size - *cursor.offset;
        const std::size_t take = std::min(remain, consumed);
        *cursor.offset += take;                        // 推进本单偏移
        consumed -= take;
        if (*cursor.offset == cursor.size)
            completeFront(conn, closeRequested);       // 本单写完，弹掉 + 回填 ticket
        else
            break;                                     // 本单没写完，剩余下次续写
        if (closeRequested)
            break;                                     // 遇到关连接单，停
    }

    return FlushStatus::Drained;
}

// Http 单：取队首 HttpStreamTask，调 writeHttpResponse 整体写
FlushStatus TransportWriter::flushHttp(Connection &conn,
                                       bool &closeRequested,
                                       std::size_t &byteBudget)
{
    auto &http = std::get<HttpStreamTask>(
        conn.transport.outboundQueue.front().payload);
    auto *response = http.response.get();
    if (!response)
    {
        completeFront(conn, closeRequested);           // 空响应，直接弹掉
        return FlushStatus::Drained;
    }

    switch (writeHttpResponse(
        conn.fd, *response, pool_, wheel_, byteBudget))
    {
    case HttpWriteResult::Done:
        completeFront(conn, closeRequested);           // 写完，弹掉 + 回填 ticket
        return FlushStatus::Drained;
    case HttpWriteResult::Blocked:
        return FlushStatus::Blocked;                   // 写满，等 EPOLLOUT
    case HttpWriteResult::Yielded:
        return FlushStatus::Yielded;                   // 本轮预算耗尽，让出 Reactor
    case HttpWriteResult::Error:
        return FlushStatus::Error;                     // 出错
    }
    return FlushStatus::Error;
}

// 弹队首：回填 completedTicket（单调取大），检 CloseConnection 标志，pop_front
void TransportWriter::completeFront(Connection &conn, bool &closeRequested)
{
    auto &task = conn.transport.outboundQueue.front();
    if (task.ticket > conn.transport.completedTicket)
        conn.transport.completedTicket = task.ticket;  // 单调回填：只增不降，驱动等待协程
    if (task.completion == OutboundCompletion::CloseConnection)
        closeRequested = true;                         // 标记请求关连接，让上层 fd_close
    task.complete(OutboundOutcome::Written);           // 终态：整单字节已交给内核 socket 缓冲
    conn.transport.outboundQueue.pop_front();          // 弹掉（task 析构时 PooledHttpResponse 归池）
}

void TransportWriter::cancelAll(Connection &conn, OutboundOutcome outcome)
{
    while (!conn.transport.outboundQueue.empty())
    {
        conn.transport.outboundQueue.front().complete(outcome);
        conn.transport.outboundQueue.pop_front();
    }
    conn.transport.pendingWriteBytes = 0;
    conn.transport.pauseByWrite = false;
    conn.state.wantWrite = false;
}

// 扣减待写计数：按本次已写字节减；若跌回低水位则撤销写背压恢复读
void TransportWriter::consumeBufferedBytes(Connection &conn, std::size_t bytes)
{
    conn.transport.pendingWriteBytes =
        bytes >= conn.transport.pendingWriteBytes
            ? 0
            : conn.transport.pendingWriteBytes - bytes;
    if (conn.transport.pauseByWrite &&
        conn.transport.pendingWriteBytes <= kWriteLowWatermark)  // 跌回低水位
    {
        conn.transport.pauseByWrite = false;
        conn.state.readPaused = conn.state.pauseByMemory;       // 撤销写背压，只保留内存背压
    }
}
