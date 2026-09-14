// =============================================================================
// 文件名：FileBody.cpp
// 职责比喻：文件响应体实现 —— 在 mmap 零拷贝与 sendfile 之间优雅切换的搬运工
//
// 【整体比喻】
// 本文件实现 FileBody 的两条发送路径：
//   1. mmap 路径：把文件映射的内存地址直接交给发送器（像把书摊开放桌上让人看）；
//   2. sendfile 路径：让内核把文件数据搬到 socket（像让快递公司直接从仓库发货）。
// 当 cleaner 回收了 mmap 映射（桌上书被收走），FileBody 会自动降级到 sendfile，
// 保证正在发送的响应不会中断。还实现了 FileCache 单例：缓存文件 fd、LRU 淘汰、
// 后台 cleaner 线程定期回收冷映射。
//
// 关键技术点（初学者重点理解）：
//   1. mmap 段借用地址：Segment.data 指向 mmapPtr+offset，不复制文件内容。
//   2. sendfile 续传：offset 是 in/out 参数，内核更新它，下次从新位置继续。
//   3. evicted 降级：原子标志位跨线程协调，cleaner 回收后发送路径自动切换。
//   4. LRU + 引用计数：cache 持有基础引用，FileBody 持有额外引用，淘汰不中断发送。
//   5. 锁外慢操作：open/fstat/mmap 都放锁外，避免阻塞其他 Reactor 线程。
// =============================================================================
#include "FileBody.h"
#include <algorithm>
#include <chrono>

// 全局线程池实例：用于异步 mmap 预热。
// 线程数取硬件并发数（CPU 核数），无法获取时默认 4 个。
ThreadPool pool(std::thread::hardware_concurrency() > 0
                    ? std::thread::hardware_concurrency()
                    : 4);

/**
 * @brief mmap 路径：从映射地址切出一段发送数据
 * @param block 段池块
 * @param max 本次最多取的字节数
 * @return 1 切出一段；0 表示已发完或需降级到 sendfile
 *
 * 【设计要点】
 * Segment 直接借用 mmap 地址，不复制文件内容。映射已被 cleaner 回收时立即关闭本响应的 mmap 策略，
 * 由 useSendfile() 引导后续调用走内核 sendfile，避免访问悬空地址。
 */
int FileBody::buildSegments(Block *block, size_t max)
{
    // Segment 直接借用 mmap 地址，不复制文件内容。映射已被 cleaner 回收时立即关闭本响应的 mmap 策略，
    // 由 useSendfile() 引导后续调用走内核 sendfile，避免访问悬空地址。
    if (remain_ == 0)
        return 0;
    // ---- 检查 mmap 是否仍然有效：evicted 表示 cleaner 已 munmap，必须降级 ----
    if (!use_mmap || file->evicted.load(std::memory_order_acquire))
    {
        use_mmap = false;   // 关闭 mmap 路径，后续走 sendfile
        return 0;
    }
    auto &seg = block->segs[block->idx++];
    auto ptr = static_cast<char *>(file->mmapPtr) + offset;  // 映射地址 + 当前偏移
    seg.data = ptr;
    seg.len = std::min(remain_, max);   // 取剩余量和本次上限的较小值
    seg.type = Segment::MMAP;           // 标记为 mmap 段，发送器知道这是映射内存
    return 1;
}

/**
 * @brief 消费已发送字节数，推进 offset 和 remain_
 * @param bytes 发送器确认已写出的字节数
 *
 * 【设计动机】TransportWriter 只传入内核实际写出的字节，因此 offset/remain_ 同步推进，
 * 即使发生部分写或在 mmap/sendfile 间降级，也不会重复或跳过文件区间。
 */
void FileBody::consume(size_t bytes)
{
    // TransportWriter 只传入内核实际写出的字节，因此 offset/remain_ 同步推进，
    // 即使发生部分写或在 mmap/sendfile 间降级，也不会重复或跳过文件区间。
    remain_ -= bytes;
    offset += bytes;
}

/**
 * @brief 是否发送完毕
 * @return remain_ 为 0 表示全部发出
 */
bool FileBody::finished() const
{
    return remain_ == 0;
}

/**
 * @brief 统计内存占用
 * @return mmap 模式返回 0（不占用户态内存）；sendfile 模式返回剩余量（虚拟统计）
 */
size_t FileBody::memoryUsage() const
{
    if (use_mmap)
        return 0;   // mmap 不占应用内存，内核按需调页
    return remain_;
}

/**
 * @brief 剩余未发送字节数
 */
size_t FileBody::remain() const
{
    return remain_;
}

/**
 * @brief sendfile 路径：内核直接把文件数据搬到 socket
 * @param sockfd 目标 socket 描述符
 * @return >0 已发送字节数；-2 表示 EAGAIN 需重试；-1 表示错误或应走 mmap
 *
 * 【sendfile 通俗解释】
 * sendfile(out_fd, in_fd, offset, count) 是 Linux 系统调用：
 * 内核直接把 in_fd（文件）的 count 字节复制到 out_fd（socket），全程在内核态，
 * 不需要把数据读到用户空间再 write 出去，省一次拷贝。
 *
 * 【返回值约定】
 * - >0：成功发出 n 字节，offset 和 remain_ 已更新；
 * - -2：EAGAIN/EWOULDBLOCK，内核发送缓冲满，协程应挂起等 EPOLLOUT；
 * - -1：错误，或 mmap 仍有效时应走 mmap 路径（发送器据此分派）。
 */
ssize_t FileBody::sendFile(int sockfd, size_t maxBytes)
{
    // sendfile 直接使用并更新 offset；EINTR 可原地重试，EAGAIN 用专用返回值交给协程等待 EPOLLOUT。
    // 当映射仍有效时返回错误标记，防止同一响应同时走 mmap 与 sendfile 两条发送路径。
    while (remain_)
    {
        // ---- 若 mmap 仍有效，返回 -1 让发送器走 mmap 路径（避免双路径并发） ----
        if (use_mmap && !file->evicted.load(std::memory_order_acquire))
            return -1;
        use_mmap = false;   // 确认走 sendfile，关闭 mmap 路径
        if (maxBytes == 0)
            return -2;      // 本轮配额已用完，由 Writer 让出 Reactor 后再继续
        constexpr size_t kSendfileChunk = 1024 * 1024;
        ssize_t n = sendfile(
            sockfd,
            file->fd,
            &offset,        // in/out：内核读取并更新偏移，下次从新位置继续
            std::min(remain_, std::min(kSendfileChunk, maxBytes)));
        if (n > 0)
        {
            remain_ -= n;   // 推进剩余量，offset 已被内核更新
            return n;
        }
        // 信号打断，尝试重新连接
        if (errno == EINTR)
            continue;       // 被信号中断，原地重试，offset 未变
        // 发不完等下一次发送
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -2;      // 内核缓冲满，返回 -2 让协程挂起等 EPOLLOUT
        // 发现错误
        return -1;          // 其他错误（EPIPE/ECONNRESET 等），连接异常
    }
    return 0;               // 全部发完
}

/**
 * @brief 是否应走 sendfile 路径
 * @return mmap 未启用或已被 evicted 时返回 true
 *
 * 【evicted 降级处理】mmap 被释放后回退到 sendfile
 */
bool FileBody::useSendfile() const
{
    // evicted 降级处理：mmap 被释放后回退到 sendfile
    return !use_mmap || file->evicted.load(std::memory_order_acquire);
}

// =============================================================================
// FileCache 实现：文件缓存单例 + 后台 cleaner 线程
// =============================================================================

/**
 * @brief 构造 FileCache 并启动 cleaner 线程
 * 【设计动机】cleaner 与进程级缓存同寿命；析构时通过 stop + join 保证线程不再访问已销毁的 cache。
 */
FileCache::FileCache()
{
    // cleaner 与进程级缓存同寿命；析构时通过 stop + join 保证线程不再访问已销毁的 cache。
    startCleaner();
}

/**
 * @brief 获取全局单例
 */
FileCache &FileCache::instace()
{
    static FileCache filecache;  // Meyers 单例，C++11 起线程安全
    return filecache;
}

/**
 * @brief 启动后台 cleaner 线程
 *
 * 【cleaner 通俗解释】
 * cleaner 像图书馆管理员，每 60 秒巡视一次书架：
 * - 遍历所有缓存条目，找出 300 秒没人访问的 mmap 映射；
 * - 把它们的 mmapPtr munmap 掉（撤下桌上的书），腾出虚拟内存；
 * - 但不关 fd（书还在书架上），下次访问时可降级到 sendfile 或重新 mmap。
 * - 用 use_count()==1 判断"只有 cache 自己持有"，正在发送的 FileBody 会增加引用计数，
 *   所以不会在借用期间被回收。
 */
void FileCache::startCleaner()
{
    cleaner = std::thread(
        [this]
        {
            while (true)
            {
                {
                    // 等待 60 秒或 stop 信号
                    std::unique_lock waitLock(cleanerWaitMtx);
                    if (cleanerCv.wait_for(
                            waitLock,
                            std::chrono::seconds(60),
                            [this] { return stop.load(); }))
                        break;  // stop=true 时退出循环
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
                            munmap(file->mmapPtr, file->size);  // 解除映射，释放虚拟内存
                            file->mmapPtr = nullptr;
                        }
                        file->mapped = false;
                        file->evicted = true;   // 标记已回收，FileBody 据此降级到 sendfile
                    }
                }
            }
        });
}

/**
 * @brief 把 time_t 格式化为 HTTP 日期字符串（RFC 1123 / RFC 7231 格式）
 * @param t UTC 时间戳
 * @return 形如 "Sun, 06 Nov 1994 08:49:37 GMT" 的字符串
 *
 * 【HTTP 日期通俗解释】HTTP 头部日期必须用 GMT 时区，格式固定，
 * 浏览器据此做缓存验证（如 If-Modified-Since）。
 */
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

/**
 * @brief 根据文件大小和修改时间生成 ETag
 * @param size 文件大小
 * @param mtime 文件修改时间
 * @return 形如 "12345-67890" 的带引号字符串
 *
 * 【ETag 通俗解释】ETag 是文件的"指纹"，浏览器下次请求时带 If-None-Match，
 * 服务端比对 ETag 一致就回 304 Not Modified，省去重传文件内容。
 */
std::string makeEtag(size_t size, time_t mtime)
{
    return "\"" +
           std::to_string(size) +
           "-" +
           std::to_string(
               mtime) +
           "\"";
}

/**
 * @brief 根据路径获取文件条目（带 LRU 缓存）
 * @param path 文件绝对路径
 * @return FileEntryPtr；失败返回 nullptr
 *
 * 【关键设计：锁内快速命中，锁外慢速 open】
 * 命中只在锁内更新 LRU；open/fstat 放到锁外，避免慢磁盘阻塞所有静态文件请求。
 */
FileEntryPtr FileCache::get(const std::string &path)
{
    // 命中只在锁内更新 LRU；open/fstat 放到锁外，避免慢磁盘阻塞所有静态文件请求。
    FileEntryPtr cached;
    {
        std::lock_guard lock(mtx);
        auto it = cache.find(path);
        if (it != cache.end())
        {
            // ---- 命中缓存：更新命中次数（上限 1000 防溢出）和访问时间 ----
            it->second->hits=std::min(it->second->hits+1ull,1000ull);
            it->second->lastVisit = time(nullptr);
            if(it->second->evicted)it->second->evicted=false;  // 重新访问，清除 evicted 标记
            lru.splice(lru.begin(), lru, it->second->lruIt);    // 移到 LRU 头部（最近使用）
            cached = it->second;
        }
    }
    if (cached)
    {
        // tryWarm 可能进入线程池，必须在 cache 锁外调用。
        tryWarm(cached);    // 命中后尝试预热（高频文件才真正 mmap）
        return cached;
    }

    // ---- 未命中：锁外 open + fstat，避免阻塞其他线程 ----
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);  // O_CLOEXEC 防止 fork+exec 泄漏 fd
    if (fd < 0)
        return nullptr;
    struct stat st;
    // 校验是普通文件且大小有效，过滤掉目录、设备文件等
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0)
    {
        close(fd);
        return nullptr;
    }

    // ---- 构造候选条目，填元数据 ----
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
            // ---- 并发竞争：另一线程已先插入，复用它的条目，本 candidate 自动 close ----
            existing->second->hits =
                std::min(existing->second->hits + 1ull, 1000ull);
            existing->second->lastVisit = time(nullptr);
            lru.splice(lru.begin(), lru, existing->second->lruIt);
            result = existing->second;
        }
        else
        {
            // ---- 正式插入新条目 ----
            lru.push_front(path);
            candidate->lruIt = lru.begin();
            cache.emplace(path, candidate);
            result = candidate;

            // 有界 LRU 防止任意路径请求让缓存永久持有文件描述符。
            // 正在发送/预热的条目由 shared_ptr 延长生命周期，淘汰不会中断当前响应。
            while (cache.size() > kMaxEntries)
            {
                // ---- 超出上限：淘汰 LRU 尾部（最久未访问）的条目 ----
                const std::string victim = lru.back();
                lru.pop_back();
                cache.erase(victim);   // shared_ptr 引用计数减一，无其他引用时 FileEntry 析构关 fd
            }
        }
    }
    tryWarm(result);    // 新插入也尝试预热
    return result;
}

/**
 * @brief 尝试为热点小文件异步建立 mmap 预热
 * @param file 文件条目
 *
 * 【预热策略】
 * 只把高频且不超过 32MB 的文件映射进用户空间，控制常驻虚拟内存；
 * 大文件继续使用 sendfile，更适合顺序传输且不挤占进程地址空间。
 *
 * 【CAS 防重复】
 * 用 warming 标志位 compare_exchange_strong，把并发命中合并为一个预热任务，
 * 避免多个 Reactor 对同一文件重复 mmap。
 */
void FileCache::tryWarm(FileEntryPtr file)
{
    // 只把高频且不超过 32MB 的文件映射进用户空间，控制常驻虚拟内存；
    // 大文件继续使用 sendfile，更适合顺序传输且不挤占进程地址空间。
    constexpr int HOT = 50;          // 命中 50 次以上才算热点
    constexpr size_t LIMIT = MiB(32); // 超过 32MB 不 mmap（虚拟内存开销大）
    if (file->mapped || file->size == 0 || file->hits < HOT || file->size > LIMIT)
        return;
    bool expected=false;
    // CAS 把并发命中合并为一个预热任务，避免多个 Reactor 对同一文件重复 mmap。
    if(!file->warming.compare_exchange_strong(expected,true))return;
    // ---- 提交异步 mmap 任务到线程池，避免阻塞 Reactor 线程 ----
    if (!pool.addTask([file]
                 {
        auto p=mmap(nullptr,file->size,PROT_READ,MAP_PRIVATE,file->fd,0);  // 只读私有映射
        if(p!=MAP_FAILED)
        {
            madvise(p,file->size,MADV_WILLNEED);  // 告诉内核即将访问，预读文件页
            file->mmapPtr=p;
            file->mapped=true;                    // 发布映射，FileBody 可用
        }
        file->warming=false; }))
    {
        // 队列满时允许后续命中重试；否则 warming 会永久卡住，热点文件永远无法预热。
        file->warming=false;   // 线程池满，重置标志让下次命中可重试
    }
}

/**
 * @brief 析构：停止 cleaner 线程并清空缓存
 */
FileCache::~FileCache()
{
    // 刷新状态，用于提醒已关闭
    stop = true;
    cleanerCv.notify_all();     // 唤醒 cleaner 让它检查 stop 标志
    // 判断线程是否还在运行，等待线程结束之后关闭
    if (cleaner.joinable())
    {
        cleaner.join();         // 等待 cleaner 线程退出，确保不再访问 cache
    }
    // 清空缓存
    cache.clear();              // shared_ptr 析构，无其他引用时关闭 fd
}

/**
 * @brief FileEntry 析构：解除映射（若仍有效）并关闭文件描述符
 *
 * 【设计要点】
 * cleaner 已回收的映射不能再次 munmap；最后一个 shared_ptr 析构时再统一关闭文件描述符。
 */
FileEntry::~FileEntry()
{
    // cleaner 已回收的映射不能再次 munmap；最后一个 shared_ptr 析构时再统一关闭文件描述符。
    if (mmapPtr && mapped && !evicted.load(std::memory_order_acquire))
    {
        munmap(mmapPtr, size);  // 仍有效的映射才解除
    }

    if (fd != -1)
    {
        close(fd);              // 关闭文件描述符，释放内核资源
    }
}
