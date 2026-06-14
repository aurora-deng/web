#ifndef HTTP_H
#define HTTP_H
#include <string>
#include <map>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <memory>
#include <deque>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

#include "server/BufferPoll/BufferPoll.h"
#include "server/Buffer/Buffer.h"
#include "server/Repsonse/RespBody.h"
// 使用共享指针优化多次move
// struct ResponseBody
// {
//     // 优化：先不申请空间，之后根据发送形式进行升级
//     std::shared_ptr<Buffer> buffer = nullptr;
//     // 使用析构函数自动释放空间，防止自己手动去找erase位置释放空间
//     ~ResponseBody()
//     {
//         if (buffer)
//             BufferPoll::instance().release(buffer);
//     }
// };
// using ResponseBodyPtr = std::shared_ptr<ResponseBody>;

enum ParseState
{
    PARSE_OK,
    PARSE_NEED_MORE,
    PARSE_ERROR
};

struct RangeInfo
{
    bool enable = false;

    size_t begin = 0;
    bool suffix = false;
    size_t end = 0;
};

struct HttpRequest
{
    bool valid = true;

    std::string method;
    std::string raw_path;
    std::string path;
    std::string query;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> querryParams;

    const char *bodyData = nullptr;

    RangeInfo range;

    size_t bodySize = 0;
    // 使用string_view这个是一个指针，不会复制内容
    std::string_view body;
};

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

struct HttpResponse
{
    int status = 200;

    std::string statusText = "OK";

    std::unordered_map<std::string, std::string> headers;

    // 使用多态的基类实现
    RespBodyPtr body = nullptr;
    bool keepAlive = true;

    // 介入chunked
    bool chunked = false;

    // 使用流式
    // StreamQueuePtr stream;

    HttpResponse();

    bool sendfile(const std::string &path, RangeInfo &range);

    // 这个其实是拼接，这样就是实现需要copy增大消耗
    // std::string toString() const;
    std::string buildHeader() const;
// 流式发送chunk
    void beginChunked();
    void writeChunk(const std::string &s);
    void endChunked();
    // 完善response函数
    void setHeader(const std::string key, std::string value);

    void text(const std::string &s);

    void html(const std::string &s);
    void json(const std::string &s);
    static HttpResponse stock404();
    static HttpResponse stock416(size_t fileSize);
};

// HttpRequest parse_request(const std::string& data);
// 优化，统一处理解析和删除，事项解析删除一体化，提高效率

ParseState try_parse_request(Buffer &buff, HttpRequest &req);

#endif