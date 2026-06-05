#include "ResponseBody.h"

bool StringBody::buildIov(std::vector<iovec> &vec)
{
    if(offset_>=body_->data.size())
        return false;
    
    iovec v;
    v.iov_base=(void*)(body_->data.data()+offset_);
    v.iov_len=body_->data.size()-offset_;

    vec.push_back(v);

    return true;
}

void StringBody::consume(size_t bytes)
{
    offset_+=bytes;
}

bool StringBody::finished() const
{
    return offset_>=body_->data.size();
}

bool ChunkedBody::buildIov(std::vector<iovec> &vec)
{
    return false;
}
