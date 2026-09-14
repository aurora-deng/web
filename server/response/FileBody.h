// =============================================================================
// 文件名：FileBody.h
// 职责比喻：文件响应体 —— 把磁盘文件"快递"给客户端的搬运工
//
// 【整体比喻】
// 当客户端请求一个大文件（图片、视频、安装包）时，不能像 StringBody 那样把整个文件
// 读进内存再发——那样内存会爆。FileBody 的做法是"借力"操作系统内核：
//   - 热点小文件：用 mmap 把文件映射到内存地址，发送器直接用这个地址发（零拷贝）；
//   - 大文件/冷文件：用 sendfile 让内核直接把文件数据搬到 socket（连用户态都不经过）。
//
// 【FileCache 文件缓存】
// 一个文件可能被很多客户端同时请求，每次都 open/read 太浪费。FileCache 把打开的文件
// 描述符和 mmap 映射缓存起来，多个 FileBody 共享同一份 FileEntry（用 shared_ptr 管理）。
// 还有个后台 cleaner 线程定期清理长时间没用的映射，回收内存。
//
// 关键技术点（初学者重点理解）：
//   1. mmap 零拷贝：把文件"贴"到进程地址空间，发送时直接用映射地址，不经过 read。
//   2. sendfile 零拷贝：内核直接把文件数据搬到 socket，连用户态拷贝都省了。
//   3. LRU + 引用计数：FileCache 用 LRU 队列管理缓存，shared_ptr 保证正在发送的文件不会被回收。
//   4. 优雅降级：mmap 映射被 cleaner 回收后，自动降级到 sendfile，不中断当前响应。
// =============================================================================
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
#include"server/buffer_pool/BufferPoll.h"
#include"server/SegmentPool/SegmentPool.h"
#include"server/thread_pool/thread_pool.h"

// 全局线程池只承担 mmap 预热，避免在 Reactor 线程执行可能产生缺页和磁盘等待的工作。
// 【设计动机】mmap 本身不读数据，但首次访问映射页会触发缺页中断（磁盘 I/O），
// 在 Reactor 线程做这件事会阻塞其他连接，所以放到独立线程池异步执行。
extern ThreadPool pool;

// =============================================================================
// FileEntry：一个打开文件的共享资源（fd + 可选 mmap 映射）
// =============================================================================
// 【FileEntry 通俗解释】
// FileEntry 是"文件资源容器"，FileCache 持有基础引用，每个正在发送的 FileBody 再持有一份。
// 这样只要还有任何 FileBody 在用这个文件，shared_ptr 的引用计数就 >0，
// FileEntry 就不会被销毁，fd 不会关闭、mmap 不会 munmap，发送安全。
// 只有当所有引用都释放后，FileEntry 才析构关闭 fd。
// =============================================================================
struct FileEntry
{
    int fd=-1;              // 打开的文件描述符（O_RDONLY），所有发送共用这一个 fd
    size_t size=0;          // 文件大小（字节），用于计算发送进度和 Content-Length
    // size/mtime 派生 HTTP 缓存元数据；当前条目代表打开时快照，便于后续扩展文件变更校验。
    time_t mtime=0;         // 文件最后修改时间，用于生成 Last-Modified / ETag 响应头

    void* mmapPtr=nullptr;  // mmap 映射的内存地址（nullptr 表示未映射或已被回收）

    // mapped 表示映射已经发布，warming 防止重复提交异步预热，evicted 表示 cleaner 已释放映射。
    // 这些原子位用于跨 Reactor/预热线程协调可见性，但 mmapPtr 的发布仍依赖现有时序约束。
    std::atomic<bool> mapped{false};     // mmap 是否已建立并可用
    std::string lastModified;            // HTTP Last-Modified 头值（RFC 1123 格式）
    bool metaReady=false;                // 元数据（size/mtime/etag）是否已就绪

    std::atomic<uint64_t> hits{0};       // 命中次数，达到阈值后触发 mmap 预热
    // hits 与 lastVisit 分别支持热点预热和冷映射回收，使小热文件走 mmap、大或冷文件走 sendfile。
    std::atomic<uint64_t> lastVisit{0};  // 最后访问时间戳，cleaner 据此判断是否回收
    std::atomic<bool> warming{false};    // 是否正在异步预热 mmap（CAS 防重复提交）
    std::string etag;                    // HTTP ETag 头值，用于条件请求（If-None-Match）
    std::atomic<bool> evicted{false};    // mmap 是否已被 cleaner 回收（触发降级）
    std::list<std::string>::iterator lruIt;  // 在 LRU 链表中的位置迭代器，O(1) 移动
    ~FileEntry();

};
using FileEntryPtr=std::shared_ptr<FileEntry>;

// =============================================================================
// FileCache：全局文件缓存（单例），复用打开的文件描述符和 mmap 映射
// =============================================================================
// 【FileCache 通俗解释】
// FileCache 像图书馆的"热门书架"：
//   - 第一次有人借某本书（请求某文件）， librarian 去 open 文件、记录元数据，放上书架；
//   - 以后再有人借同一本书，直接从书架拿（命中缓存），不用重新 open；
//   - 借的人多（hits 高）的书，预先复制一本放桌上（mmap 预热），借阅更快；
//   - 长期没人借的书（lastVisit 超过 300 秒），从桌上撤下（munmap）腾地方，但 fd 还留着；
//   - 书架容量有限（kMaxEntries=1024），满了就按 LRU 淘汰最久没借的。
//
// 【线程安全】cache/lru 受 mtx 保护；返回 shared_ptr 后调用方可在锁外发送，
// 因为 shared_ptr 引用计数保证 FileEntry 存活。cleaner 只回收 mmap，不销毁仍被引用的 FileEntry。
// =============================================================================
class FileCache
{
private:
    static constexpr size_t kMaxEntries = 1024;  // 缓存上限：最多缓存 1024 个文件条目
    std::unordered_map<std::string, FileEntryPtr>cache;  // path → FileEntry 的映射表
    std::thread cleaner;                  // 后台清理线程，定期回收冷映射
    std::list<std::string> lru;           // LRU 链表：最近访问的在头部，最久没访问的在尾部
    std::atomic<bool> stop{false};        // 析构时置 true，通知 cleaner 退出
    std::mutex mtx;                       // 保护 cache 和 lru 的互斥锁
    std::mutex cleanerWaitMtx;            // cleaner 等待用的独立锁，避免与 cache 锁冲突
    std::condition_variable cleanerCv;    // cleaner 定时唤醒 / 析构通知的条件变量

public:
    FileCache();
    // 全局变量filecache，让全局所有线程使用
    /**
     * @brief 获取全局单例 FileCache 实例
     * @return FileCache 引用（进程内唯一）
     * 【设计动机】文件缓存必须全局共享，否则不同线程各存一份会浪费 fd 和内存。
     * 用 Meyers 单例（static 局部变量）保证线程安全初始化。
     */
    static FileCache &instace();

    /**
     * @brief 启动后台 cleaner 线程，定期回收长时间未访问的 mmap 映射
     */
    void startCleaner();

    /**
     * @brief 根据路径获取文件条目（带 LRU 缓存）
     * @param path 文件绝对路径
     * @return FileEntryPtr；文件不存在或无权限返回 nullptr
     *
     * 【流程】
     * 1. 锁内查 cache：命中则更新 LRU 和 lastVisit，返回；
     * 2. 未命中则锁外 open+fstat（避免慢磁盘阻塞其他请求）；
     * 3. 锁内插入新条目，处理并发 miss（只保留一个，另一个自动 close）；
     * 4. 调 tryWarm 尝试预热 mmap（高频小文件才真正映射）。
     */
    FileEntryPtr get(const std::string &path);
    // 达到热点阈值且文件较小时异步建立 mmap；未预热或已回收时由 FileBody 自动退回 sendfile。
    void tryWarm(FileEntryPtr file);
    ~FileCache();
};

// =============================================================================
// FileBody：文件响应体，一次响应的文件区间视图
// =============================================================================
// 【FileBody 通俗解释】
// FileBody 不"拥有"文件，它只是"借用" FileEntry 的一份 shared_ptr，记录本次响应要发文件的
// 哪一段（offset + remain_）。就像快递员拿的是"取件单"——知道去哪个仓库（FileEntry）、
// 取哪段货物（offset~offset+remain），但仓库本身归 FileCache 管。
//
// 【双路径发送】
//   - mmap 路径：use_mmap=true 时，buildSegments 用 mmapPtr 生成 MMAP 类型段，用户态零拷贝；
//   - sendfile 路径：mmap 失效（evicted）时，useSendfile() 返回 true，发送器调 sendFile() 走内核零拷贝。
//   - 统一偏移：offset/remain_ 在两条路径间共享，切换后能从同一位置续传。
// =============================================================================
class FileBody : public RespBody
{
public:
    FileEntryPtr file;        // 共享的文件条目（fd + mmap），shared_ptr 保证发送期间存活
    off_t offset=0;           // 当前发送偏移（mmap 和 sendfile 共用，sendfile 会更新它）
    size_t remain_=0;         // 剩余未发送字节数
    std::string filePath;     // 文件路径（调试/日志用）
    size_t filesize;          // 文件总大小
    size_t begin;             // Range 请求起始偏移（支持断点续传）
    size_t end;               // Range 请求结束偏移

    // use_mmap 是本响应的策略快照；若 cleaner 标记 evicted，发送时必须降级，不能再解引用旧映射。
    bool use_mmap=false;      // 本次响应是否走 mmap 路径（构造时根据 file 状态决定）

    // 仅 mmap 路径构建 Segment；sendfile 路径由 TransportWriter 根据 useSendfile() 分派。
    int buildSegments(Block* block,size_t max) override;

    void consume(size_t bytes) override;
    bool finished() const override;
    size_t memoryUsage() const override;
    size_t remain()const override;
    ssize_t sendFile(int sockfd, size_t maxBytes) override;
    bool useSendfile()const override;
};
#endif
