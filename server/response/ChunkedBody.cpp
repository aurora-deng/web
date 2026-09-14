// =============================================================================
// 文件名：ChunkedBody.cpp
// 职责比喻：分块传输实现 —— 把队列里的块"三段拆解"后逐段发给发送器
//
// 【整体比喻】
// 本文件实现 ChunkedBody 的取货流程。每个分块在内存里是三段（prefix 长度行 + data 数据 +
// suffix CRLF），发送器来取货时，我们从队首块开始，按 sent 进度切出尚未发出的部分。
// 因为一段块可能跨越 prefix/data/suffix 三个区域，所以要用偏移算出当前应该发哪一段。
// consume() 按已发字节推进 sent，一块发完就 pop，继续下一块。
//
// 关键技术点（初学者重点理解）：
//   1. 三段跨越计算：sent 是块内累计偏移，需依次减去 prefix/data 长度算出当前在哪段。
//   2. 锁内拷贝指针：buildSegments 在锁内把队首块的数据指针拷出来，锁外构造 Segment，
//      减少持锁时间。但需注意 chunks.front() 在 consume 前不会被 pop，指针稳定。
//   3. 部分发送续传：consume 按字节推进 sent，可能一块分多次发完，不重发不漏发。
//   4. 终止块：finish() push 一个 prefix="0\r\n\r\n" 的空块，标志传输结束。
// =============================================================================
#include "ChunkedBody.h"
#include "server/buffer_pool/BufferPoll.h"

/**
 * @brief 向队列追加一个分块并唤醒消费者
 * @param c 待追加的分块
 * 【设计动机】加锁 push 后调 wakeup，让挂起的发送协程知道有新数据可发。
 */
void StreamQueue::pushChunk(ChunkBolck c)
{
    {
        std::lock_guard lock(mtx);

        chunks.push_back(std::move(c));
    }
    if (wakeup)
        wakeup();   // 队列从空变非空，唤醒等待的发送协程
}

// bool ChunkedBody::next(std::vector<Segment> &seg, size_t max)
// {
//     seg.clear();
//     // 局部加锁取东西
//     std::string prefix_copy, suffix_copy;
//     const char *data_ptr = nullptr;
//     size_t data_len = 0;
//     size_t sent_copy = 0;
//     {
//         std::lock_guard lk(stream->mtx);
//         if (stream->chunks.empty())
//         {
//             return false;
//         }
//         auto &c = stream->chunks.front();
//         prefix_copy = c.prefix;
//         suffix_copy = c.suffix;
//         sent_copy = c.sent;
//         if (c.data && c.data->buffer_)
//         {
//             data_ptr = c.data->buffer_->peek();
//             data_len = c.data->buffer_->readableBytes();
//         }
//     }

//     // 构建iov
//     size_t used = 0;
//     // 使用变量储存大部分数据
//     size_t sent = sent_copy;
//     // 头部大小
//     size_t p = prefix_copy.size();
//     ;
//     // 内容体文件大小
//     size_t d = data_len;
//     // 结尾大小
//     size_t s = suffix_copy.size();

//     if (sent < p)
//     {
//         seg.push_back({(prefix_copy.data() + sent), p - sent});
//         used += p - sent;
//     }
//     sent -= p;
//     if (sent < d)
//     {
//         seg.push_back({(data_ptr + sent), d - sent});
//         used += d - sent;
//     }
//     sent -= d;
//     if (sent < s)
//     {
//         seg.push_back({(suffix_copy.data() + sent), s - sent});
//         used += s - sent;
//     }
//     return !seg.empty();
// }

/**
 * @brief 从队首块的三段中切出未发送部分生成 Segment
 * @param block 段池块
 * @param max 本次最多取的字节数；三段合计绝不超过该预算
 * @return >0 已填入段数；false/0 表示队列空
 *
 * 【三段跨越计算】
 * sent 是块内累计已发偏移，依次判断：
 *   - sent < prefix.size()：还在 prefix 段，取 prefix+sent 起 prefix.size()-sent 字节；
 *   - sent 减去 prefix 后 < data_len：在 data 段，取 data+剩余偏移；
 *   - sent 减去 prefix+data 后 < suffix.size()：在 suffix 段，取 suffix+剩余偏移。
 * 每段生成一个 BUFFER 类型 Segment，最多生成 3 个段。
 */
int ChunkedBody::buildSegments(Block *block, size_t max)
{
    if (!block || max == 0)
        return 0;

    std::lock_guard lk(stream->mtx);
    if (stream->chunks.empty())
        return 0;

    // 生产者只在 deque 尾部追加；队首由本 Writer 的 consume 串行推进。
    // 因此可直接借用队首字符串/Buffer 的地址，避免指向局部副本造成悬空。
    auto &chunk = stream->chunks.front();
    const size_t prefixSize = chunk.prefix.size();
    const char *data = nullptr;
    size_t dataSize = 0;
    if (chunk.data && chunk.data->buffer_)
    {
        data = chunk.data->buffer_->peek();
        dataSize = chunk.data->buffer_->readableBytes();
    }
    const size_t suffixSize = chunk.suffix.size();
    size_t budget = max;

    auto append = [&](const char *base, size_t size, size_t offset)
    {
        if (!base || offset >= size || budget == 0)
            return;
        const size_t length = std::min(size - offset, budget);
        Segment &segment = block->segs[block->idx++];
        segment.data = base + offset;
        segment.len = length;
        segment.type = Segment::BUFFER;
        budget -= length;
    };

    const size_t sent = chunk.sent;
    const size_t prefixOffset = std::min(sent, prefixSize);
    append(chunk.prefix.data(), prefixSize, prefixOffset);

    const size_t afterPrefix =
        sent > prefixSize ? sent - prefixSize : 0;
    const size_t dataOffset = std::min(afterPrefix, dataSize);
    append(data, dataSize, dataOffset);

    const size_t afterData =
        afterPrefix > dataSize ? afterPrefix - dataSize : 0;
    const size_t suffixOffset = std::min(afterData, suffixSize);
    append(chunk.suffix.data(), suffixSize, suffixOffset);

    return block->idx;
}

// bool ChunkedBody::buildIov(std::vector<iovec> &vec, size_t maxBytes)
// {
//     // 局部加锁取东西
//     std::string prefix_copy, suffix_copy;
//     const char* data_ptr = nullptr;
//     size_t data_len = 0;
//     size_t sent_copy = 0;
//     {
//         std::lock_guard lk(stream->mtx);
//         if (stream->chunks.empty())
//         {
//             return false;
//         }
//         auto &c = stream->chunks.front();
//         prefix_copy = c.prefix;
//         suffix_copy = c.suffix;
//         sent_copy = c.sent;
//         if (c.data && c.data->buffer_)
//         {
//             data_ptr = c.data->buffer_->peek();
//             data_len = c.data->buffer_->readableBytes();
//         }
//     }

//     // 构建iov
//     size_t used = 0;
//     // 使用变量储存大部分数据
//     size_t sent = sent_copy;
//     // 头部大小
//     size_t p = prefix_copy.size();;
//     // 内容体文件大小
//     size_t d = data_len;
//     // 结尾大小
//     size_t s = suffix_copy.size();

//     if (sent < p)
//     {
//         vec.push_back({(void *)(prefix_copy.data() + sent), p - sent});
//         used += p - sent;
//     }
//     sent -= p;
//     if (sent < d)
//     {
//         vec.push_back({(void *)(data_ptr + sent), d - sent});
//         used += d - sent;
//     }
//     sent -= d;
//     if (sent < s)
//     {
//        vec.push_back({(void *)(suffix_copy.data() + sent), s - sent});
//         used += s - sent;
//     }
//     return !vec.empty();
// }

/**
 * @brief 消费已发送字节数，推进各块的 sent，发完的块出队
 * @param bytes 本次已发出的总字节数
 *
 * 【设计动机】加锁判断损耗
 * 可能一次发出跨越多个块的字节，循环处理：
 *   1. 计算队首块剩余可消费量 = 块总长 - sent；
 *   2. 取 min(剩余, bytes) 推进 sent；
 *   3. sent == 块总长则 pop，继续下一块；否则 break（本块还没发完）。
 */
void ChunkedBody::consume(size_t bytes)
{
    // 加锁判断损耗
    std::lock_guard lk(stream->mtx);
    while (bytes && !stream->chunks.empty())
    {
        auto &c = stream->chunks.front();
        size_t dataLen = (c.data && c.data->buffer_) ? c.data->buffer_->readableBytes() : 0;
        size_t total = c.prefix.size() + dataLen + c.suffix.size();  // 块三段总长
        size_t remain = total - c.sent;                              // 本块剩余未发
        // 统计消耗的内存
        size_t use = std::min(remain, bytes);                        // 本次能消费多少
        c.sent += use;
        bytes -= use;
        if (c.sent == total)
        {
            stream->chunks.pop_front();   // 本块发完，出队
        }
        else
        {
            break;                        // 本块还没发完，后续字节留给下次
        }
    }
}

/**
 * @brief 是否全部发送完毕
 * @return done 为 true（已 push 终止块）且队列为空
 */
bool ChunkedBody::finished() const
{
    std::lock_guard lock(stream->mtx);
    return done && stream->chunks.empty();
}

/**
 * @brief 向队列追加一个分块（业务侧调用）
 * @param c 分块数据
 * 队列从空变非空时触发 wakeup 回调唤醒发送协程
 */
void ChunkedBody::push(ChunkBolck c)
{
    bool notify = false;
    {
        std::lock_guard lock(stream->mtx);
        notify = stream->chunks.empty();   // 队列空→非空才需要唤醒
        stream->chunks.push_back(std::move(c));
    }
    if (wakeup && notify)
        wakeup();                          // 唤醒挂起的发送协程
}

/**
 * @brief 追加 HTTP chunked 终止块，标志传输结束
 * 【终止块格式】prefix="0\r\n\r\n"，data 为空 StringBody，suffix 为空
 * push 后置 done=true，finished() 在队列清空后返回 true
 */
void ChunkedBody::finish()
{
    ChunkBolck c;
    c.prefix = "0\r\n\r\n";   // 长度为 0 的块 + 结束 CRLF
    c.data = std::make_shared<StringBody>(BufferPoll::instance().acquire());  // 空 data
    c.suffix = "";
    bool notify = false;
    {
        std::lock_guard lock(stream->mtx);
        if (done)
            return;           // finish 幂等，禁止重复终止块
        notify = stream->chunks.empty();
        stream->chunks.push_back(std::move(c));
        done = true;          // 与终止块入队在同一临界区提交，避免丢失唤醒
        stream->finished = true;
    }
    if (wakeup && notify)
        wakeup();
}

/**
 * @brief 队列中所有块剩余未发送字节数
 * @return 遍历所有块累加 (块总长 - sent)
 */
size_t ChunkedBody::remain() const
{
    std::lock_guard lock(stream->mtx);
    size_t total = 0;
    for (auto &c : stream->chunks)
    {
        size_t dataLen = (c.data && c.data->buffer_) ? c.data->buffer_->readableBytes() : 0;
        size_t chunk_total = c.prefix.size() + dataLen + c.suffix.size();
        total += (chunk_total > c.sent) ? (chunk_total - c.sent) : 0;
    }
    return total;
}

/**
 * @brief ChunkedBody 不支持 sendfile（数据在内存队列）
 * @return 固定 -1
 */
ssize_t ChunkedBody::sendFile(int, size_t)
{
    return -1;
}

/**
 * @brief 统计队列占用内存
 * @return 所有块三段长度之和（不含 sent 已发部分，但这里简单统计总量）
 */
size_t ChunkedBody::memoryUsage() const
{
    size_t total = 0;
    for (auto &c : stream->chunks)
    {
        size_t dataLen = (c.data && c.data->buffer_) ? c.data->buffer_->readableBytes() : 0;
        total += c.prefix.size() + dataLen + c.suffix.size();
    }
    return total;
}

/**
 * @brief 标识本响应体为分块传输
 * @return 固定 true，告诉发送器走 chunked 路径
 */
bool ChunkedBody::Chunked() const
{
    return true;
}
