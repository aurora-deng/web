#include "HeaderBody.h"

HeaderBody::~HeaderBody()
{
    if (buffer_)
    {
        BufferPoll::instance().release(buffer_);
    }
}

void HeaderBody::append(const std::string &s)
{
    buffer_->append(s.data(), s.size());
}

int HeaderBody::buildSegments(Block *block, size_t max)
{
    auto remain = buffer_->readableBytes() - offset;

    if (remain == 0)
        return 0;
    auto n = std::min(remain, max);
    auto &seg = block->segs[block->idx++];
    seg.type = Segment::BUFFER;
    seg.data = buffer_->peek() + offset;
    seg.len = n;
    return 1;
}

void HeaderBody::consume(size_t bytes)
{
    offset+=bytes;
}

bool HeaderBody::finished() const 
{
    return offset>=buffer_->readableBytes();
}

size_t HeaderBody::remain() const
{
    return buffer_->readableBytes()-offset;
}
ssize_t HeaderBody::sendFile(int) 
{
    return -1;
}

size_t HeaderBody::memoryUsage() const
{
    if(!buffer_)return 0;
    return buffer_->readableBytes();
}