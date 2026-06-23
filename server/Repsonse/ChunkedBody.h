#ifndef CHUNKED_BODY_H
#define CHUNKED_BODY_H
#include<deque>
#include<functional>
#include <sys/uio.h>
#include<algorithm>
#include<memory>
#include <utility>

#include "RespBody.h"
#include"StringBody.h"
#include "server/Buffer/Buffer.h"

struct ChunkBolck
{
    std::string prefix;
    std::shared_ptr<StringBody> data = nullptr;
    std::string suffix = "\r\n";
    size_t sent = 0;

};

// 使用流式传递chunk,单独使用流式发送，形成多生成单消费的高效模式
struct StreamQueue
{
    std::mutex mtx;
    std::deque<ChunkBolck> chunks;

    bool finished = false;

    // 故意对于push来写一个pushchunk是为了后续扩展方便，有利于集成一些策略
    void pushChunk(ChunkBolck c);
    // 增加回调函数使得自己唤醒自己
    std::function<void()> wakeup;
};
using StreamQueuePtr = std::shared_ptr<StreamQueue>;


class ChunkedBody : public RespBody
{
public:
    ~ChunkedBody()override=default;
    bool buildIov(std::vector<iovec> &vec, size_t maxBytes) override;
    void consume(size_t bytes) override;
    bool finished() const override;
    void finish() override;
    size_t remain() const override;
    ssize_t sendFile(int fd)override;
    size_t memoryUsage() const override;

    void push(ChunkBolck c);
    // 由于使用的是共享指针，所以依旧使用通过共享指针的数量来判断是否使用chunk
    std::function<void()> wakeup;
    // 段错误修复处：stream 必须默认初始化，否则 buildIov/consume/push 等访问空指针
    StreamQueuePtr stream = std::make_shared<StreamQueue>();

private:

    size_t chunkIndex = 0;
    bool done = false;
};

#endif