#include"StringBody.h"

StringBody::~StringBody()
{
    if(buffer_)
    {
        BufferPoll::instance().release(buffer_);
    }
}

// bool StringBody::next(std::vector<Segment> &seg, size_t max)
// {
//     seg.clear();
//     if(finished())return false;

//     size_t n=std::min(remain(),max);
//     seg.push_back({buffer_->peek()+offset_,n});
//     return true;
// }

// bool StringBody::buildIov(std::vector<iovec> &vec, size_t maxBytes)
// {
//     if(finished())return false;

//     iovec v;
//     v.iov_base=(void*)(buffer_->peek()+offset_);
//     v.iov_len=std::min(maxBytes,remain());
//     vec.push_back(v);
//     return true;
// }

int StringBody::buildSegments(Block *block,size_t max)
{
    if(finished())return 0;
    size_t n=std::min(remain(),max);
    Segment& seg=block->segs[block->idx++];
    seg.data=buffer_->peek()+offset_;
    seg.len=n;
    seg.type=Segment::CONST;
    return 1;
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