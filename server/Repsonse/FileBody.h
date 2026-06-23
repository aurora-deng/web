#ifndef FILE_BODY_H
#define FILE_BODY_H
#include<unordered_map>
#include<mutex>
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


#include"RespBody.h"
#include"server/Buffer/Buffer.h"
#include"server/BufferPoll/BufferPoll.h"
#include"server/SegmentPool/SegmentPool.h"
#include"server/threadpoll/thread_pool.h"

extern ThreadPool pool; // 公用main函数全局的线程池
// 优化：直接通过对应的fileEntry的析构函数直接删除，同时通过共享指针来纠错
struct FileEntry
{
    int fd=-1;
    size_t size=0;
    // 用于记录文件内容何时变化
    time_t mtime=0;

    void* mmapPtr=nullptr;

    bool mapped=false;
    std::string lastModified;
    bool metaReady=false;

    std::atomic<uint64_t> hits{0};
    // 记录服务器什么时候访问过，用途：冷热判断，释放 mmap，预热判
    std::atomic<uint64_t> lastVisit{0};
    bool warning=false;
    std::string etag;
    // 用于判断是否已经被munmap释放
    std::atomic<bool> evicted{false};
    ~FileEntry();

};
using FileEntryPtr=std::shared_ptr<FileEntry>;

// 使用filecache统一管理filefd,有锁不允许拷贝
class FileCache
{
private:
    std::unordered_map<std::string, FileEntryPtr>cache;
    std::thread cleaner;
    std::atomic<bool> stop;
    std::mutex mtx;

public:
    FileCache();
    // 全局变量filecache，让全局所有线程使用
    static FileCache &instace();
    void startCleaner();
    FileEntryPtr get(const std::string &path);
    // 判断是否需要持续放到缓存中，主要根据命中来判断
    void tryWarm(FileEntryPtr file);
    // 优化：将删除统一交给自己，结束即析构自动删除，防止统一删除后继续沿用，没有立即删除
    ~FileCache();
};

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

    // ________________添加mmap来提高file优化内存使用————————————————————————————————
    bool use_mmap=false;
    // 判断是否需要发送file,通过共享指针的数量来判断
    // bool next(std::vector<Segment>&seg,size_t max) override;
    // 使用该函数池化内存分配
    int buildSegments(Block* block,size_t max) override;

    // bool buildIov(std::vector<iovec> &vec,size_t maxBytes) override;
    void consume(size_t bytes) override;
    bool finished() const override;
    size_t memoryUsage() const override;
    size_t remain()const override;
    ssize_t sendFile(int sockfd) override;
    bool useSendfile()const override;
    // ~FileBody();    //用来和filepath在filecache进行统一关闭使用：优化使用entry自己的析构自己释放

};
#endif