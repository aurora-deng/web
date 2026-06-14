#include"StringBody.h"

StringBody::~StringBody()
{
    if(buffer_)
    {
        BufferPoll::instance().release(buffer_);
    }
}

bool StringBody::buildIov(std::vector<iovec> &vec, size_t maxBytes)
{
    if(finished())return false;

    iovec v;
    v.iov_base=(void*)(buffer_->peek()+offset_);
    v.iov_len=std::min(maxBytes,remain());
    vec.push_back(v);
    return true;
}

void StringBody::consume(size_t bytes) 
{
    offset_+=bytes;
}

bool StringBody::finished() const 
{
    return offset_>=buffer_->readableBytes();
}

size_t StringBody::remain() const
{
    return buffer_->readableBytes()-offset_;
}
ssize_t StringBody::sendFile(int) 
{
    return -1;
}

size_t StringBody::memoryUsage() const
{
    if(!buffer_)return 0;
    return buffer_->readableBytes();
}