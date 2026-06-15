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
    std::string prefix_copy, suffix_copy;
    const char* data_ptr = nullptr;
    size_t data_len = 0;
    size_t sent_copy = 0;
    {
        std::lock_guard lk(stream->mtx);
        if (stream->chunks.empty())
        {
            return false;
        }
        auto &c = stream->chunks.front();
        prefix_copy = c.prefix;
        suffix_copy = c.suffix;
        sent_copy = c.sent;
        if (c.data && c.data->buffer_)
        {
            data_ptr = c.data->buffer_->peek();
            data_len = c.data->buffer_->readableBytes();
        }
    }

    // 构建iov
    size_t used = 0;
    // 使用变量储存大部分数据
    size_t sent = sent_copy;
    // 头部大小
    size_t p = prefix_copy.size();;
    // 内容体文件大小
    size_t d = data_len;
    // 结尾大小
    size_t s = suffix_copy.size();

    if (sent < p)
    {
        vec.push_back({(void *)(prefix_copy.data() + sent), p - sent});
        used += p - sent;
    }
    sent -= p;
    if (sent < d)
    {
        vec.push_back({(void *)(data_ptr + sent), d - sent});
        used += d - sent;
    }
    sent -= d;
    if (sent < s)
    {
       vec.push_back({(void *)(suffix_copy.data() + sent), s - sent});
        used += s - sent;
    }
    return !vec.empty();
}

void ChunkedBody::consume(size_t bytes)
{
    // 加锁判断损耗
    std::lock_guard lk(stream->mtx);
    while (bytes && !stream->chunks.empty())
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
    bool notify = false;
    {
        std::lock_guard lock(stream->mtx);
        notify = stream->chunks.empty();
        stream->chunks.push_back(std::move(c));
    }
    if (wakeup && notify)
        wakeup();
}

void ChunkedBody::finish()
{
    ChunkBolck c;
    c.prefix = "0\r\n\r\n";
    c.data=std::make_shared<StringBody>();
    c.data->buffer_ = BufferPoll::instance().acquire();
    c.suffix = "";
    push(std::move(c));
    done = true;
}

size_t ChunkedBody::remain() const
{
    size_t total = 0;
    for (auto &c : stream->chunks)
    {
        size_t chunk_total = c.prefix.size() + c.data->buffer_->readableBytes() + c.suffix.size();
        total += (chunk_total > c.sent) ? (chunk_total - c.sent) : 0;
    }
    return total;
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
