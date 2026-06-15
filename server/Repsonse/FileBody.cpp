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



FileCache &FileCache::instace()
{
    static FileCache filecache;
    return filecache;
}

bool FileCache::get(const std::string &path, FileEntry &out)
{
    std::lock_guard lock(mtx);

    auto it = cache.find(path);

    if (it != cache.end())
    {
        it->second.refCount++;
        out = it->second;
        return true;
    }
    int fd = open(path.c_str(), O_RDONLY);

    if (fd < 0)
        return false;

    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        close(fd);
        return false;
    }

    FileEntry entry;
    entry.size = st.st_size;
    entry.mtime = st.st_mtime;
    entry.fd = fd;
    entry.refCount = 1;

    cache[path] = entry;
    out = entry;
    return true;
}

void FileCache::put(const std::string &path)
{
    std::lock_guard lock(mtx);

    auto it = cache.find(path);

    if (it == cache.end())
        return;

    it->second.refCount--;

    if (it->second.refCount <= 0)
    {
        close(it->second.fd);
        cache.erase(it);
    }
}

FileCache::~FileCache()
{
    for (auto &[k, v] : cache)
    {
        close(v.fd);
    }
}
