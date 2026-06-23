#ifndef HEADER_H
#define HEADER_H
#include <vector>
#include <sys/uio.h>
#include <algorithm>
#include <memory>

#include "RespBody.h"
#include "server/Buffer/Buffer.h"
#include "server/BufferPoll/BufferPoll.h"
#include "server/SegmentPool/SegmentPool.h"
class HeaderBody : public RespBody
{
public:
    std::shared_ptr<Buffer> buffer_;
    size_t offset = 0;

    explicit HeaderBody()
    {
        buffer_ = BufferPoll::instance().acquire();
    }
    // 新增：接收 shared_ptr<Buffer> 的构造函数
    explicit HeaderBody(std::shared_ptr<Buffer> buf)
    {
        buffer_ = std::move(buf);
    }
    ~HeaderBody();
    void append(const std::string &s);
    int buildSegments(Block *block, size_t max) override;
    void consume(size_t bytes) override;
    bool finished() const override;
    size_t remain() const override;
    ssize_t sendFile(int fd) override;
    size_t memoryUsage() const override;
};

#endif