#include "FileBody.h"

bool FileBody::buildIov(std::vector<iovec> &vec, size_t)
{
    return false;
}

void FileBody::consume(size_t bytes)
{
    remain_-=bytes;
    offset+=bytes;
}

bool FileBody::finished() const
{
    return remain_==0;
}

size_t FileBody::memoryUsage() const
{
    return 0;
}

size_t FileBody::remain() const
{
    return remain_;
}

ssize_t FileBody::sendFile(int sockfd)
{
    while (remain_)
    {
        ssize_t n=sendfile(sockfd,fd,&offset,remain_);
        if(n>0)
        {
            remain_-=n;
            return n;
        }
        // 信号打断，尝试重新连接
        if(errno==EINTR)continue;
        // 发不完等下一次发送
        if (errno == EAGAIN || errno == EWOULDBLOCK)return -2;
        // 发现错误
        return -1;
            
    }
    return 0;
    
}

FileBody::~FileBody()
{
    if(!filePath.empty())
    {
        FileCache::instace().put(filePath);
    }
}
