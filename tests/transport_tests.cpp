#include <gtest/gtest.h>

#include "server/Route/Router.h"
#include "server/SegmentPool/SegmentPool.h"
#include "server/http/http.h"
#include "server/timer/TimeWheel.h"
#include "server/transport/Connection.h"
#include "server/transport/OutboundQueue.h"
#include "server/transport/TransportWriter.h"

#include <atomic>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unordered_map>
#include <unistd.h>

namespace {

class SocketPair
{
public:
    SocketPair()
    {
        EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets_), 0);
    }
    ~SocketPair()
    {
        if (sockets_[0] >= 0)
            close(sockets_[0]);
        if (sockets_[1] >= 0)
            close(sockets_[1]);
    }

    int writer() const { return sockets_[0]; }
    int reader() const { return sockets_[1]; }

private:
    int sockets_[2]{-1, -1};
};

std::string readAll(int fd)
{
    std::string wire;
    char buffer[512];
    while (const ssize_t size = read(fd, buffer, sizeof(buffer)))
    {
        if (size < 0)
            break;
        wire.append(buffer, static_cast<std::size_t>(size));
    }
    return wire;
}

} // namespace

TEST(TransportWriterTest, PreservesEncodedTaskFifo)
{
    SocketPair sockets;
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    Connection connection;
    connection.fd = sockets.writer();

    ASSERT_EQ(
        writer.enqueue(connection, OutboundTask::encoded("first")),
        EnqueueResult::Ok);
    ASSERT_EQ(
        writer.enqueue(connection, OutboundTask::encoded("-second")),
        EnqueueResult::Ok);
    EXPECT_EQ(writer.flush(connection).status, FlushStatus::Drained);
    ASSERT_EQ(shutdown(sockets.writer(), SHUT_WR), 0);
    EXPECT_EQ(readAll(sockets.reader()), "first-second");
}

TEST(TransportWriterTest, YieldsAfterPerConnectionWriteBudget)
{
    SocketPair sockets;
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    Connection connection;
    connection.fd = sockets.writer();
    auto receipt = std::make_shared<OutboundReceipt>();

    ASSERT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(
                "abcdefgh",
                0,
                OutboundCompletion::None,
                receipt)),
        EnqueueResult::Ok);

    const auto first = writer.flush(connection, 3);
    EXPECT_EQ(first.status, FlushStatus::Yielded);
    EXPECT_EQ(first.bytesWritten, 3U);
    EXPECT_EQ(receipt->outcome(), OutboundOutcome::Pending);
    EXPECT_FALSE(connection.transport.outboundQueue.empty());

    char prefix[3]{};
    ASSERT_EQ(read(sockets.reader(), prefix, sizeof(prefix)), 3);
    EXPECT_EQ(std::string(prefix, sizeof(prefix)), "abc");

    const auto second = writer.flush(connection, 5);
    EXPECT_EQ(second.status, FlushStatus::Drained);
    EXPECT_EQ(second.bytesWritten, 5U);
    EXPECT_EQ(receipt->outcome(), OutboundOutcome::Written);
    ASSERT_EQ(shutdown(sockets.writer(), SHUT_WR), 0);
    EXPECT_EQ(readAll(sockets.reader()), "defgh");
}

TEST(TransportWriterTest, ReturnsPooledHttpResponseExactlyOnce)
{
    SocketPair sockets;
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    Connection connection;
    connection.fd = sockets.writer();

    auto *response = responsePool.acquire();
    const std::size_t availableAfterAcquire = responsePool.available();
    response->keepAlive = false;
    response->text("transport writer");
    response->buildHeader();

    ASSERT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::http(PooledHttpResponse(response), 7)),
        EnqueueResult::Ok);
    EXPECT_EQ(writer.flush(connection).status, FlushStatus::Drained);
    EXPECT_EQ(connection.transport.completedTicket, 7U);
    EXPECT_EQ(responsePool.available(), availableAfterAcquire + 1);
}

TEST(TransportWriterTest, AppliesWriteBudgetToHttpHeaderAndBody)
{
    SocketPair sockets;
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    Connection connection;
    connection.fd = sockets.writer();
    auto receipt = std::make_shared<OutboundReceipt>();

    auto *response = responsePool.acquire();
    response->keepAlive = true;
    response->text(std::string(80, 'h'));
    response->buildHeader();
    ASSERT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::http(
                PooledHttpResponse(response),
                9,
                OutboundCompletion::None,
                receipt)),
        EnqueueResult::Ok);

    const auto first = writer.flush(connection, 8);
    EXPECT_EQ(first.status, FlushStatus::Yielded);
    EXPECT_EQ(first.bytesWritten, 8U);
    EXPECT_EQ(receipt->outcome(), OutboundOutcome::Pending);

    for (int turns = 0;
         turns < 64 && !connection.transport.outboundQueue.empty();
         ++turns)
        writer.flush(connection, 8);

    EXPECT_TRUE(connection.transport.outboundQueue.empty());
    EXPECT_EQ(connection.transport.completedTicket, 9U);
    EXPECT_EQ(receipt->outcome(), OutboundOutcome::Written);
}

TEST(ChunkedBodyTest, BorrowsStableSegmentsWithinRequestedBudget)
{
    ChunkedBody body;
    ChunkBolck chunk;
    chunk.prefix = "abcdef";
    chunk.suffix = "xy";
    body.push(std::move(chunk));

    Block first{};
    ASSERT_TRUE(body.buildSegments(&first, 3));
    ASSERT_EQ(first.idx, 1);
    EXPECT_EQ(first.segs[0].len, 3U);
    EXPECT_EQ(std::string(first.segs[0].data, first.segs[0].len), "abc");
    body.consume(3);

    Block second{};
    ASSERT_TRUE(body.buildSegments(&second, 4));
    std::string wire;
    std::size_t offered = 0;
    for (int index = 0; index < second.idx; ++index)
    {
        wire.append(second.segs[index].data, second.segs[index].len);
        offered += second.segs[index].len;
    }
    EXPECT_EQ(offered, 4U);
    EXPECT_EQ(wire, "defx");
    body.consume(4);
    EXPECT_EQ(body.remain(), 1U);
}

TEST(TransportWriterTest, AppliesHighWatermarkBackpressure)
{
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    Connection connection;
    connection.fd = 1;

    ASSERT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(std::string(3 * 1024 * 1024, 'a'))),
        EnqueueResult::Ok);
    EXPECT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(std::string(3 * 1024 * 1024, 'b'))),
        EnqueueResult::Backpressure);
    EXPECT_TRUE(connection.transport.pauseByWrite);
    EXPECT_TRUE(connection.state.readPaused);
}

TEST(TransportWriterTest, ReceiptRecordsBackpressureAndCancellationOnce)
{
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    Connection connection;

    ASSERT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(std::string(3 * 1024 * 1024, 'a'))),
        EnqueueResult::Ok);

    auto rejected = std::make_shared<OutboundReceipt>();
    EXPECT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(
                std::string(2 * 1024 * 1024, 'b'),
                0,
                OutboundCompletion::None,
                rejected)),
        EnqueueResult::Backpressure);
    EXPECT_EQ(rejected->outcome(), OutboundOutcome::Backpressured);
    EXPECT_FALSE(rejected->complete(OutboundOutcome::Written));

    auto cancelled = std::make_shared<OutboundReceipt>();
    ASSERT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(
                "queued",
                0,
                OutboundCompletion::CloseConnection,
                cancelled)),
        EnqueueResult::Ok);
    writer.cancelAll(connection, OutboundOutcome::Closed);
    EXPECT_EQ(cancelled->outcome(), OutboundOutcome::Closed);
    EXPECT_TRUE(connection.transport.outboundQueue.empty());
    EXPECT_EQ(connection.transport.pendingWriteBytes, 0U);
}

TEST(TransportWriterTest, CloseTaskIsWriteBarrierForLaterTasks)
{
    SocketPair sockets;
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    Connection connection;
    connection.fd = sockets.writer();
    auto closing = std::make_shared<OutboundReceipt>();
    auto late = std::make_shared<OutboundReceipt>();

    ASSERT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(
                "close",
                0,
                OutboundCompletion::CloseConnection,
                closing)),
        EnqueueResult::Ok);
    ASSERT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(
                "late",
                0,
                OutboundCompletion::None,
                late)),
        EnqueueResult::Ok);

    const auto result = writer.flush(connection, 64);
    EXPECT_TRUE(result.closeRequested);
    EXPECT_EQ(closing->outcome(), OutboundOutcome::Written);
    EXPECT_EQ(late->outcome(), OutboundOutcome::Pending);
    writer.cancelAll(connection, OutboundOutcome::Closed);
    EXPECT_EQ(late->outcome(), OutboundOutcome::Closed);
    ASSERT_EQ(shutdown(sockets.writer(), SHUT_WR), 0);
    EXPECT_EQ(readAll(sockets.reader()), "close");
}

TEST(TransportWriterTest, RejectsOversizedFirstTaskWithoutOverflow)
{
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    Connection connection;

    EXPECT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(
                std::string(kWriteHighWatermark + 1, 'x'))),
        EnqueueResult::Backpressure);
    EXPECT_TRUE(connection.transport.outboundQueue.empty());

    // 人工把计数放到边界，验证判断使用减法，不会因 size_t 相加上溢绕过限制。
    connection.transport.pendingWriteBytes = kWriteHighWatermark;
    EXPECT_EQ(
        writer.enqueue(connection, OutboundTask::encoded("x")),
        EnqueueResult::Backpressure);

    EXPECT_EQ(
        writer.enqueue(
            connection,
            OutboundTask::encoded(
                "close", 0, OutboundCompletion::CloseConnection)),
        EnqueueResult::Ok);
}

TEST(OutboundQueueTest, BoundsMailboxGloballyAndPerConnection)
{
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    OutboundPostLimits limits;
    limits.maxTasks = 3;
    limits.maxBytes = 12;
    limits.maxTasksPerConnection = 2;
    limits.maxBytesPerConnection = 8;
    OutboundQueue queue(writer, {}, {}, limits);
    std::atomic<bool> notified{true}; // 避免测试依赖真实 eventfd

    EXPECT_EQ(queue.post(10, 100, OutboundTask::encoded("1234"), -1, notified),
              EnqueueResult::Ok);
    EXPECT_EQ(queue.post(10, 100, OutboundTask::encoded("5678"), -1, notified),
              EnqueueResult::Ok);
    EXPECT_EQ(queue.post(10, 100, OutboundTask::encoded("x"), -1, notified),
              EnqueueResult::Backpressure); // 单连接任务/字节上限
    EXPECT_EQ(queue.post(11, 101, OutboundTask::encoded("abcd"), -1, notified),
              EnqueueResult::Ok);
    EXPECT_EQ(queue.post(12, 102, OutboundTask::encoded("x"), -1, notified),
              EnqueueResult::Backpressure); // Reactor 全局任务/字节上限
    EXPECT_EQ(queue.pendingTaskCount(), 3U);
    EXPECT_EQ(queue.pendingWireBytes(), 12U);

    std::unordered_map<int, std::unique_ptr<Connection>> connections;
    auto first = std::make_unique<Connection>();
    first->fd = 10;
    first->id = 100;
    connections.emplace(10, std::move(first));
    auto second = std::make_unique<Connection>();
    second->fd = 11;
    second->id = 101;
    connections.emplace(11, std::move(second));

    const auto stats = queue.processPending(connections);
    EXPECT_EQ(stats.enqueued, 3U);
    EXPECT_EQ(stats.backpressured, 0U);
    EXPECT_EQ(stats.stale, 0U);
    EXPECT_EQ(queue.pendingTaskCount(), 0U);
    EXPECT_EQ(queue.pendingWireBytes(), 0U);

    // 一批任务取走后邮箱配额归还，下一批可以继续准入。
    EXPECT_EQ(queue.post(10, 100, OutboundTask::encoded("next"), -1, notified),
              EnqueueResult::Ok);
}

TEST(OutboundQueueTest, DropsStaleGenerationDuringDrain)
{
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    OutboundQueue queue(writer, {}, {});
    std::atomic<bool> notified{true};
    auto receipt = std::make_shared<OutboundReceipt>();
    ASSERT_EQ(queue.post(
                  20,
                  999,
                  OutboundTask::encoded(
                      "late",
                      0,
                      OutboundCompletion::None,
                      receipt),
                  -1,
                  notified),
              EnqueueResult::Ok);

    std::unordered_map<int, std::unique_ptr<Connection>> connections;
    auto replacement = std::make_unique<Connection>();
    replacement->fd = 20;
    replacement->id = 1000;
    connections.emplace(20, std::move(replacement));

    const auto stats = queue.processPending(connections);
    EXPECT_EQ(stats.enqueued, 0U);
    EXPECT_EQ(stats.stale, 1U);
    EXPECT_EQ(receipt->outcome(), OutboundOutcome::Stale);
    EXPECT_TRUE(connections.at(20)->transport.outboundQueue.empty());
}

TEST(TransportWriterTest, SharesImmutableBroadcastBuffer)
{
    SegmentPool segments;
    TimerWheel wheel(16, 5);
    TransportWriter writer(segments, wheel);
    Connection first;
    Connection second;
    auto bytes = std::make_shared<const std::string>("shared-frame");

    ASSERT_EQ(
        writer.enqueue(first, OutboundTask::sharedEncoded(bytes)),
        EnqueueResult::Ok);
    ASSERT_EQ(
        writer.enqueue(second, OutboundTask::sharedEncoded(bytes)),
        EnqueueResult::Ok);
    EXPECT_EQ(bytes.use_count(), 3);
}
