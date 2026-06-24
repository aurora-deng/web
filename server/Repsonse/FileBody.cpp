#include "FileBody.h"

// bool FileBody::next(std::vector<Segment> &seg, size_t max)
// {
//     seg.clear();
//     if (!use_mmap)
//         return false;
//     if (!remain_ <= 0)
//         return false;
//     size_t n = std::min(remain_, max);
//     auto ptr = static_cast<char *>(file->mmapPtr) + offset;
//     seg.push_back({ptr, n});
//     return true;
// }

int FileBody::buildSegments(Block *block, size_t max)
{
    if (remain_ == 0)
        return 0;
    if (!use_mmap || file->evicted.load(std::memory_order_acquire))
    {
        use_mmap = false;
        return 0;
    }
    auto &seg = block->segs[block->idx++];
    auto ptr = static_cast<char *>(file->mmapPtr) + offset;
    seg.data = ptr;
    seg.len = std::min(remain_, max);
    seg.type = Segment::MMAP;
    return 1;
}
// bool FileBody::buildIov(std::vector<iovec> &vec, size_t maxBytes)
// {
//     if (!use_mmap)
//         return false;
//     if (!remain_ <= 0)
//         return false;
//     size_t n = std::min(remain_, maxBytes);
//     auto ptr = static_cast<char *>(file->mmapPtr) + offset;
//     vec.push_back({ptr, n});
//     return true;
// }

void FileBody::consume(size_t bytes)
{
    remain_ -= bytes;
    offset += bytes;
}

bool FileBody::finished() const
{
    return remain_ == 0;
}

size_t FileBody::memoryUsage() const
{
    if (use_mmap)
        return 0;
    return remain_;
}

size_t FileBody::remain() const
{
    return remain_;
}

ssize_t FileBody::sendFile(int sockfd)
{
    while (remain_)
    {
        if (use_mmap && !file->evicted.load(std::memory_order_acquire))
            return -1;
        use_mmap = false;
        ssize_t n = sendfile(sockfd, file->fd, &offset, remain_);
        if (n > 0)
        {
            remain_ -= n;
            return n;
        }
        // 信号打断，尝试重新连接
        if (errno == EINTR)
            continue;
        // 发不完等下一次发送
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -2;
        // 发现错误
        return -1;
    }
    return 0;
}

bool FileBody::useSendfile() const
{
    // evicted 降级处理：mmap 被释放后回退到 sendfile
    return !use_mmap || file->evicted.load(std::memory_order_acquire);
}

FileCache::FileCache()
{
    startCleaner();
}

FileCache &FileCache::instace()
{
    static FileCache filecache;
    return filecache;
}

void FileCache::startCleaner()
{
    cleaner = std::thread(
        [this]
        {
            while (!stop)
            {
                sleep(60);
                auto now = time(nullptr);
                std::lock_guard lock(mtx);
                for (auto &[path, file] : cache)
                {
                    // 降级决策
                    if (file->mapped &&file.use_count()==1&& now - file->lastVisit > 300)
                    {
                        if (file->mmapPtr)
                        {
                            munmap(file->mmapPtr, file->size);
                            file->mmapPtr = nullptr;
                        }
                        file->mapped = false;
                        file->evicted = true;
                    }
                }
            }
        });
}

static inline std::string httpDate(time_t t)
{
    char buf[128];
    tm tmv;
    // 将时间戳 time_t（从 1970-01-01 UTC 秒数）转换成 UTC/GMT 零时区 的年月日时分秒结构体 struct tm；
    gmtime_r(&t, &tmv);
    // 按照自定义格式，把 struct tm 时间结构体格式化输出成可读字符串。
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
    return buf;
}

std::string makeEtag(size_t size, time_t mtime)
{
    return "\"" +
           std::to_string(size) +
           "-" +
           std::to_string(
               mtime) +
           "\"";
}
FileEntryPtr FileCache::get(const std::string &path)
{
    auto e = std::make_shared<FileEntry>();
    {
        std::lock_guard lock(mtx);

        auto it = cache.find(path);

        if (it != cache.end())
        {
            it->second->hits=std::min(it->second->hits+1ull,1000ull);
            it->second->lastVisit = time(nullptr);
            if(it->second->evicted)it->second->evicted=false;
            tryWarm(it->second);
            return it->second;
        }
        int fd = open(path.c_str(), O_RDONLY);

        if (fd < 0)
            return nullptr;

        struct stat st;
        if (fstat(fd, &st) < 0)
        {
            close(fd);
            return nullptr;
        }

        e->fd = fd;
        e->size = st.st_size;
        e->mtime = st.st_mtime;
        e->etag = makeEtag(e->size, e->mtime);
        e->lastModified = httpDate(e->mtime);
        e->hits=std::min(e->hits+1ull,1000ull);
        e->lastVisit = time(nullptr);
        e->metaReady = true;
        cache[path] = e;
    }
    // 静态缓存
    tryWarm(e);
    return e;
}

void FileCache::tryWarm(FileEntryPtr file)
{
    constexpr int HOT = 50;
    constexpr size_t LIMIT = MB(32);
    if (file->mapped)
        return;
    bool expected=false;
    // 优化，避免重复预热，比较置换函数，如果warming和expected相同，则warming变成true，函数返回ture修改成功继续
    // 如果warming和expected不相同，即warming本身就是true，不修改值，直接退出
    if(!file->warming.compare_exchange_strong(expected,true))return;
    if (file->hits < HOT)
        return;
    if (file->size > LIMIT)
        return;
    pool.addTask([file]
                 {
        auto p=mmap(nullptr,file->size,PROT_READ,MAP_PRIVATE,file->fd,0);
        if(p!=MAP_FAILED)
        {
            madvise(p,file->size,MADV_WILLNEED);
            file->mmapPtr=p;
            file->mapped=true;
        }
        file->warming=false; });
}

FileCache::~FileCache()
{
    // 刷新状态，用于提醒已关闭
    stop = true;
    // 判断线程是否还在运行，等待线程结束之后关闭
    if (cleaner.joinable())
    {
        cleaner.join();
    }
    // 清空缓存
    cache.clear();
}

FileEntry::~FileEntry()
{
    // evicted 降级处理：mmap 已被 cleaner 释放，不能重复 munmap
    if (mmapPtr && mapped && !evicted.load(std::memory_order_acquire))
    {
        munmap(mmapPtr, size);
    }

    if (fd != -1)
    {
        close(fd);
    }
}
