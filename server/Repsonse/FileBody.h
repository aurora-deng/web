#ifndef FILE_BODY_H
#define FILE_BODY_H
#include<unordered_map>
#include<mutex>
#include <condition_variable>
#include<string>
#include<string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/uio.h>
#include<memory>
#include<unistd.h>
#include<sys/mman.h>
#include <atomic>
#include<thread>
#include<list>

#include"RespBody.h"
#include"server/Buffer/Buffer.h"
#include"server/BufferPoll/BufferPoll.h"
#include"server/SegmentPool/SegmentPool.h"
#include"server/threadpoll/thread_pool.h"

// 全局线程池只承担 mmap 预热，避免在 Reactor 线程执行可能产生缺页和磁盘等待的工作。
extern ThreadPool pool;

// FileEntry 是打开文件与可选 mmap 映射的共享所有者。FileCache 持有基础引用，
// 每个正在发送的 FileBody 再持有一份，保证 fd/映射不会在响应发送途中析构。
struct FileEntry
{
    int fd=-1;
    size_t size=0;
    // size/mtime 派生 HTTP 缓存元数据；当前条目代表打开时快照，便于后续扩展文件变更校验。
    time_t mtime=0;

    void* mmapPtr=nullptr;

    // mapped 表示映射已经发布，warming 防止重复提交异步预热，evicted 表示 cleaner 已释放映射。
    // 这些原子位用于跨 Reactor/预热线程协调可见性，但 mmapPtr 的发布仍依赖现有时序约束。
    std::atomic<bool> mapped{false};
    std::string lastModified;
    bool metaReady=false;

    std::atomic<uint64_t> hits{0};
    // hits 与 lastVisit 分别支持热点预热和冷映射回收，使小热文件走 mmap、大或冷文件走 sendfile。
    std::atomic<uint64_t> lastVisit{0};
    std::atomic<bool> warming{false};
    std::string etag;
    std::atomic<bool> evicted{false};
    std::list<std::string>::iterator lruIt;
    ~FileEntry();

};
using FileEntryPtr=std::shared_ptr<FileEntry>;

// FileCache 集中复用打开文件和元数据。cache/lru 受 mtx 保护，返回 shared_ptr 后调用方可在锁外发送；
// cleaner 只回收 mmap，不直接销毁仍被响应引用的 FileEntry。
class FileCache
{
private:
    static constexpr size_t kMaxEntries = 1024;
    std::unordered_map<std::string, FileEntryPtr>cache;
    std::thread cleaner;
    std::list<std::string> lru;
    std::atomic<bool> stop{false};
    std::mutex mtx;
    std::mutex cleanerWaitMtx;
    std::condition_variable cleanerCv;

public:
    FileCache();
    // 全局变量filecache，让全局所有线程使用
    static FileCache &instace();
    void startCleaner();
    FileEntryPtr get(const std::string &path);
    // 达到热点阈值且文件较小时异步建立 mmap；未预热或已回收时由 FileBody 自动退回 sendfile。
    void tryWarm(FileEntryPtr file);
    ~FileCache();
};

// FileBody 保存一次响应的区间和消费偏移，不拥有底层文件资源而共享 FileEntry。
// mmap 路径可生成内存 Segment，sendfile 路径由内核搬运；统一的 remain_/offset
// 让两种策略切换后仍能从同一位置续传，便于学习零拷贝方案的取舍。
class FileBody : public RespBody
{
public:
    FileEntryPtr file;
    off_t offset=0;
    size_t remain_=0;
    std::string filePath;
    size_t filesize;
    size_t begin;
    size_t end;

    // use_mmap 是本响应的策略快照；若 cleaner 标记 evicted，发送时必须降级，不能再解引用旧映射。
    bool use_mmap=false;

    // 仅 mmap 路径构建 Segment；sendfile 路径由 ResponseSender 根据 useSendfile() 分派。
    int buildSegments(Block* block,size_t max) override;

    void consume(size_t bytes) override;
    bool finished() const override;
    size_t memoryUsage() const override;
    size_t remain()const override;
    ssize_t sendFile(int sockfd) override;
    bool useSendfile()const override;
};
#endif