#include "FileBody.h"
#include <algorithm>
#include <chrono>

ThreadPool pool(std::thread::hardware_concurrency() > 0
                    ? std::thread::hardware_concurrency()
                    : 4);

int FileBody::buildSegments(Block *block, size_t max)
{
    // Segment 直接借用 mmap 地址，不复制文件内容。映射已被 cleaner 回收时立即关闭本响应的 mmap 策略，
    // 由 useSendfile() 引导后续调用走内核 sendfile，避免访问悬空地址。
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
void FileBody::consume(size_t bytes)
{
    // ResponseSender 只传入内核实际写出的字节，因此 offset/remain_ 同步推进，
    // 即使发生部分写或在 mmap/sendfile 间降级，也不会重复或跳过文件区间。
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
    // sendfile 直接使用并更新 offset；EINTR 可原地重试，EAGAIN 用专用返回值交给协程等待 EPOLLOUT。
    // 当映射仍有效时返回错误标记，防止同一响应同时走 mmap 与 sendfile 两条发送路径。
    while (remain_)
    {
        if (use_mmap && !file->evicted.load(std::memory_order_acquire))
            return -1;
        use_mmap = false;
        constexpr size_t kSendfileChunk = 1024 * 1024;
        ssize_t n = sendfile(
            sockfd,
            file->fd,
            &offset,
            std::min(remain_, kSendfileChunk));
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
    // cleaner 与进程级缓存同寿命；析构时通过 stop + join 保证线程不再访问已销毁的 cache。
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
            while (true)
            {
                {
                    std::unique_lock waitLock(cleanerWaitMtx);
                    if (cleanerCv.wait_for(
                            waitLock,
                            std::chrono::seconds(60),
                            [this] { return stop.load(); }))
                        break;
                }
                auto now = time(nullptr);
                std::lock_guard lock(mtx);
                for (auto &[path, file] : cache)
                {
                    // 仅缓存自身持有条目且超过冷却时间时释放 mmap；正在发送的 FileBody 会增加引用计数，
                    // 因而不会在其借用映射地址期间进入该分支。fd 继续保留，以便无缝降级到 sendfile。
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
    // 命中只在锁内更新 LRU；open/fstat 放到锁外，避免慢磁盘阻塞所有静态文件请求。
    FileEntryPtr cached;
    {
        std::lock_guard lock(mtx);
        auto it = cache.find(path);
        if (it != cache.end())
        {
            it->second->hits=std::min(it->second->hits+1ull,1000ull);
            it->second->lastVisit = time(nullptr);
            if(it->second->evicted)it->second->evicted=false;
            lru.splice(lru.begin(), lru, it->second->lruIt);
            cached = it->second;
        }
    }
    if (cached)
    {
        // tryWarm 可能进入线程池，必须在 cache 锁外调用。
        tryWarm(cached);
        return cached;
    }

    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return nullptr;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
    {
        close(fd);
        return nullptr;
    }

    auto candidate = std::make_shared<FileEntry>();
    candidate->fd = fd;
    candidate->size = static_cast<size_t>(st.st_size);
    candidate->mtime = st.st_mtime;
    candidate->etag = makeEtag(candidate->size, candidate->mtime);
    candidate->lastModified = httpDate(candidate->mtime);
    candidate->hits = 1;
    candidate->lastVisit = time(nullptr);
    candidate->metaReady = true;

    FileEntryPtr result;
    {
        std::lock_guard lock(mtx);
        // 两个并发 miss 可能同时完成 open；只发布一个，另一个 candidate 离开作用域时自动 close。
        auto existing = cache.find(path);
        if (existing != cache.end())
        {
            existing->second->hits =
                std::min(existing->second->hits + 1ull, 1000ull);
            existing->second->lastVisit = time(nullptr);
            lru.splice(lru.begin(), lru, existing->second->lruIt);
            result = existing->second;
        }
        else
        {
            lru.push_front(path);
            candidate->lruIt = lru.begin();
            cache.emplace(path, candidate);
            result = candidate;

            // 有界 LRU 防止任意路径请求让缓存永久持有文件描述符。
            // 正在发送/预热的条目由 shared_ptr 延长生命周期，淘汰不会中断当前响应。
            while (cache.size() > kMaxEntries)
            {
                const std::string victim = lru.back();
                lru.pop_back();
                cache.erase(victim);
            }
        }
    }
    tryWarm(result);
    return result;
}

void FileCache::tryWarm(FileEntryPtr file)
{
    // 只把高频且不超过 32MB 的文件映射进用户空间，控制常驻虚拟内存；
    // 大文件继续使用 sendfile，更适合顺序传输且不挤占进程地址空间。
    constexpr int HOT = 50;
    constexpr size_t LIMIT = MB(32);
    if (file->mapped || file->size == 0 || file->hits < HOT || file->size > LIMIT)
        return;
    bool expected=false;
    // CAS 把并发命中合并为一个预热任务，避免多个 Reactor 对同一文件重复 mmap。
    if(!file->warming.compare_exchange_strong(expected,true))return;
    if (!pool.addTask([file]
                 {
        auto p=mmap(nullptr,file->size,PROT_READ,MAP_PRIVATE,file->fd,0);
        if(p!=MAP_FAILED)
        {
            madvise(p,file->size,MADV_WILLNEED);
            file->mmapPtr=p;
            file->mapped=true;
        }
        file->warming=false; }))
    {
        // 队列满时允许后续命中重试；否则 warming 会永久卡住，热点文件永远无法预热。
        file->warming=false;
    }
}

FileCache::~FileCache()
{
    // 刷新状态，用于提醒已关闭
    stop = true;
    cleanerCv.notify_all();
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
    // cleaner 已回收的映射不能再次 munmap；最后一个 shared_ptr 析构时再统一关闭文件描述符。
    if (mmapPtr && mapped && !evicted.load(std::memory_order_acquire))
    {
        munmap(mmapPtr, size);
    }

    if (fd != -1)
    {
        close(fd);
    }
}
