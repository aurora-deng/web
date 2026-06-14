#include "ChunkedBody.h"

void StreamQueue::pushChunk(ChunkBolck c)
{
    {
        std::lock_guard lock(mtx);

        chunks.push_back(std::move(c));
    }
    if (wakeup)
        wakeup();
}

bool ChunkedBody::buildIov(std::vector<iovec> &vec, size_t maxBytes)
{
    // 局部加锁取东西
    std::lock_guard lk(stream->mtx);
    if (stream->chunks.empty())
    {
        return false;
    }

    auto &c = stream->chunks.front();
    stream->mtx.unlock();

    // 构建iov
    size_t used = 0;
    // 使用变量储存大部分数据
    size_t sent = c.sent;
    // 头部大小
    size_t p = c.prefix.size();
    // 内容体文件大小
    size_t d = c.data->buffer_->readableBytes();
    // 结尾大小
    size_t s = c.suffix.size();

    if (sent < p)
    {
        vec.push_back({(void *)(c.prefix.data() + c.sent), c.prefix.size() - c.sent});
        used += p - sent;
    }
    sent -= p;
    if (sent < d)
    {
        vec.push_back({(void *)(c.data->buffer_->peek() + sent), d - sent});
        used += d - sent;
    }
    sent -= d;
    if (sent < s)
    {
        vec.push_back({(void *)(c.suffix.data() + sent), s - sent});
        used += s - sent;
    }
    return !vec.empty();
}

void ChunkedBody::consume(size_t bytes)
{
    // 加锁判断损耗
    std::lock_guard lk(stream->mtx);
    while (bytes &&!stream->chunks.empty())
    {
        auto &c = stream->chunks.front();
        size_t total = c.prefix.size() + c.data->buffer_->readableBytes() + c.suffix.size();
        size_t remain = total - c.sent;
        // 统计消耗的内存
        size_t use = std::min(remain, bytes);
        c.sent += use;
        bytes -= use;
        if (c.sent == total)
        {
            stream->chunks.pop_front();
        }
        else
        {
            break;
        }
    }
}

bool ChunkedBody::finished() const
{
    return done && stream->chunks.empty();
}

void ChunkedBody::push(ChunkBolck c)
{
    bool notify=false;
    {
        std::lock_guard lock(stream->mtx);
        notify=stream->chunks.empty();
        stream->chunks.push_back(std::move(c));
    }
    if (wakeup&&notify)
        wakeup();
}

void ChunkedBody::finish()
{
    ChunkBolck c;
    c.prefix = "0\r\n\r\n";
    c.data->buffer_= BufferPoll::instance().acquire();;
    c.suffix = "";
    push(std::move(c));
    done = true;
    
}

ssize_t ChunkedBody::sendFile(int fd)
{
    return -1;
}

size_t ChunkedBody::memoryUsage() const
{
    size_t total = 0;
    for (auto &c : stream->chunks)
    {
        total += c.prefix.size() + c.data->buffer_->readableBytes() + c.suffix.size();
    }
    return total;
}
