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


#include"RespBody.h"
#include"server/Buffer/Buffer.h"
#include"server/BufferPoll/BufferPoll.h"


struct FileEntry
{
    int fd;
    size_t size;
    time_t mtime;

    int refCount;
};

// 使用filecache统一管理filefd,有锁不允许拷贝
class FileCache
{
private:
    std::unordered_map<std::string, FileEntry> cache;

    std::mutex mtx;

public:
    // 全局变量filecache，让全局所有线程使用
    static FileCache &instace();

    bool get(const std::string &path, FileEntry &out);

    void put(const std::string &path);
    ~FileCache();
};

class FileBody : public RespBody
{
public:
    int fd=-1;
    off_t offset=0;
    size_t remain_=0;
    std::string filePath;
    size_t filesize;
    size_t begin;
    size_t end;
    // 判断是否需要发送file,通过共享指针的数量来判断
   
    bool buildIov(std::vector<iovec> &vec,size_t) override;
    void consume(size_t bytes) override;
    bool finished() const override;
    size_t memoryUsage() const override;
    size_t remain()const override;
    ssize_t sendFile(int sockfd) override;

    ~FileBody();    //用来和filepath在filecache进行统一关闭使用
};
#endif