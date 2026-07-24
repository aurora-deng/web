
#include "http.h"
// 统一小写
static inline std::string toLower(std::string s)
{
    for (char &c : s)
        c = std::tolower((unsigned char)c);
    return s;
}

HttpResponse::HttpResponse()
{
}
inline std::string httpDate(time_t t)
{
    char buf[128];
    tm tmv;
    // 将时间戳 time_t（从 1970-01-01 UTC 秒数）转换成 UTC/GMT 零时区 的年月日时分秒结构体 struct tm；
    gmtime_r(&t, &tmv);
    // 按照自定义格式，把 struct tm 时间结构体格式化输出成可读字符串。
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
    return buf;
}

inline std::string makeEtag(size_t size, time_t mtime)
{
    return "\"" +
           std::to_string(size) +
           "-" +
           std::to_string(
               mtime) +
           "\"";
}

// 既然使用sendfile就是在静态文件发送，可能range出现问题，但是还是静态文件，因此状态就要变成206
bool HttpResponse::sendfile(const std::string &path, const HttpRequest &req, RangeInfo &range)
{
    struct stat st;

    if (stat(path.c_str(), &st) == -1)
        return false;

    // // 整体作用：以只读方式打开磁盘文件，拿到文件句柄 fd，用于后续 sendfile 发送文件
    // int fd = open(path.c_str(), O_RDONLY);

    // if (fd == -1)
    //     return false;

    // 使用filecache优化处理open和close
    FileEntryPtr file = FileCache::instace().get(path);
    if (!file)
    {
        return false;
    }
    // 判断是否复用
    auto et = req.headers.find("if-nonoe-match");
    if (et != req.headers.end() && et->second == file->etag)
    {
        status = 304;
        statusText =
            "Not Modified";

        body =
            nullptr;

        return true;
    }

    size_t fileSize = file->size;
    int sendEnd = fileSize - 1, sendBegin = 0;

    if (range.enable)
    {
        sendEnd = std::min(range.end, size_t(fileSize - 1));
        if (!range.suffix)
        {
            sendBegin = range.begin;
        }
        else
        {

            sendBegin = fileSize - sendEnd;
        }
        // 越界处理
        if (sendBegin > fileSize || sendBegin > sendEnd)
        {
            *this = stock416(fileSize);
            return true;
        }
        // 更新Status
        status = 206;
        statusText = "Partial Content";
        headers["Accept-Ranges"] = "bytes";
    }
    headers["ETag"] = file->etag;

    headers["Last-Modified"] = file->lastModified;
    headers["Cache-Control"] = "public,max-age=3600";

    constexpr size_t SMALL = KB(128);
    constexpr size_t MMAP = MB(16);

    // 小文件
    if (fileSize < SMALL)
    {
        auto buf = BufferPoll::instance().acquire();
        size_t len = sendEnd - sendBegin + 1;
        buf->ensureWrite(len);
        ssize_t n = pread(file->fd, buf->beginWrite(), len, sendBegin);
        if (n <= 0)
        {
            BufferPoll::instance().release(buf);
            return false;
        }
        buf->writePos += n;
        auto s = std::make_shared<StringBody>();
        s->buffer_ = buf;
        body = s;
        return true;
    }
    auto b = std::make_shared<FileBody>();
    // 大文件
    if (fileSize < MMAP)
    {
        if (file->mapped && file->mmapPtr && !file->evicted.load(std::memory_order_acquire))
        {
            // FileCache 已经完成 mmap，直接使用
            b->use_mmap = true;
        }
        else if (!file->mapped && !file->warming)
        {
            // FileCache 还没有 mmap，在请求路径中做 mmap
            // 注意：这里的 mmap 由 FileEntry 管理，cleaner 可以释放
            void *p = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, file->fd, 0);
            if (p != MAP_FAILED)
            {
                file->mmapPtr = p;
                file->mapped = true;
                b->use_mmap = true;
            }
        }
    }
    b->file = file;
    b->begin = sendBegin;
    b->end = sendEnd;
    b->offset = sendBegin;
    b->remain_ = sendEnd - sendBegin + 1;
    b->filePath = path;
    b->filesize = fileSize;
    this->body = b;

    return true;
}

void HttpResponse::beginChunked()
{
    chunked = true;

    body = std::make_shared<ChunkedBody>();
    headers["Transfer-Encoding"] = "chunked";
}

void HttpResponse::writeChunk(const std::string &s)
{

    auto chunk = std::dynamic_pointer_cast<ChunkedBody>(body);
    if (!chunk)
        return;
    ChunkBolck c;
    std::stringstream ss;
    ss << std::hex << s.size();

    c.prefix = ss.str() + "\r\n";

    auto buf = BufferPoll::instance().acquire();
    buf->append(s.data(), s.size());
    auto body = std::make_shared<StringBody>(buf);
    c.data = body;

    c.suffix = "\r\n";

    chunk->push(std::move(c));
}

void HttpResponse::endChunked()
{
    if (body)
        body->finish();
}

std::string HttpResponse::buildHeader() const
{
    std::string res;
   
    res +=
        "HTTP/1.1 " + std::to_string(status) +
        " " +
        statusText +
        "\r\n";
    // 优化使用chunk
    // bool hasCL = false;
    for (auto &[k, v] : headers)
    {
        std::string lk = toLower(k);
        if (lk == "content-length" || lk == "transfer-encoding")
        {
            continue;
        }

        res += k + ": " + v + "\r\n";
        //     hasCL = true;
    }

    // if (!hasCL)

    if (keepAlive)
    {
        res += "Connection: keep-alive\r\n";
    }
    else
    {
        res += "Connection: close\r\n";
    }
    // chunked标记
    if (chunked)
    {
        res += "Transfer-Encoding: chunked\r\n";
    }
    else
    {
        auto file = std::dynamic_pointer_cast<FileBody>(body);

        if (file)
        {
            res += "Accept-Ranges: bytes\r\n";

            if (status == 206)
            {
                res += "Content-Range: bytes ";
                res += std::to_string(file->begin);
                res += "-";
                res += std::to_string(file->end);
                res += "/";
                res += std::to_string(file->filesize);
                res += "\r\n";
            }

            if (status == 416)
            {
                res += "Content-Range: bytes ";
                res += "*/" + std::to_string(file->filesize) + "\r\n";
            }
            else
            {
                res += "Content-Length: " + std::to_string(file->remain_) + "\r\n";
            }
        }
        else
        {
            size_t memSize = body ? body->memoryUsage() : 0;
            res += "Content-Length: " + std::to_string(memSize) + "\r\n";
        }
    }

    // 头部结束
    res += "\r\n";
    return res;
}

void HttpResponse::buildHeader()
{
    auto buf=BufferPoll::instance().acquire();
   
    buf->append
        ("HTTP/1.1 " + std::to_string(status) +
        " " +
        statusText +
        "\r\n");
    // 优化使用chunk
    // bool hasCL = false;
    for (auto &[k, v] : headers)
    {
        std::string lk = toLower(k);
        if (lk == "content-length" || lk == "transfer-encoding")
        {
            continue;
        }

        buf->append (k + ": " + v + "\r\n");
        //     hasCL = true;
    }

    // if (!hasCL)

    if (keepAlive)
    {
        buf->append( "Connection: keep-alive\r\n");
    }
    else
    {
        buf->append( "Connection: close\r\n");
    }
    // chunked标记
    if (chunked)
    {
        buf->append ("Transfer-Encoding: chunked\r\n");
    }
    else
    {
        auto file = std::dynamic_pointer_cast<FileBody>(body);

        if (file)
        {
            buf->append( "Accept-Ranges: bytes\r\n");

            if (status == 206)
            {
                buf->append ("Content-Range: bytes ");
                buf->append( std::to_string(file->begin));
                buf->append ("-");
                buf->append (std::to_string(file->end));
                buf->append ("/");
                buf->append (std::to_string(file->filesize));
                buf->append ("\r\n");
            }

            if (status == 416)
            {
                buf->append( "Content-Range: bytes ");
                buf->append ("*/" + std::to_string(file->filesize) + "\r\n");
            }
            else
            {
                buf->append( "Content-Length: " + std::to_string(file->remain_) + "\r\n");
            }
        }
        else
        {
            size_t memSize = body ? body->memoryUsage() : 0;
            buf->append ("Content-Length: " + std::to_string(memSize) + "\r\n");
        }
    }

    // 头部结束
    buf->append ("\r\n");
    HeaderBody_=std::make_shared<HeaderBody>(buf);
}

void HttpResponse::setHeader(const std::string key, std::string value)
{
    headers[key] = std::move(value);
}

void HttpResponse::text(const std::string &s)
{

    auto buf = BufferPoll::instance().acquire();
    buf->append(s.data(), s.size());
    body = std::make_shared<StringBody>(buf);
    headers["Content-Type"] = "text/plain";
}

void HttpResponse::html(const std::string &s)
{

    auto buf = BufferPoll::instance().acquire();
    buf->append(s.data(), s.size());
    body = std::make_shared<StringBody>(buf);
    headers["Content-Type"] = "text/html";
}

void HttpResponse::json(const std::string &s)
{

    auto buf = BufferPoll::instance().acquire();
    buf->append(s.data(), s.size());
    body = std::make_shared<StringBody>(buf);
    headers["Content-Type"] = "application/json; charset=utf-8";
}

HttpResponse HttpResponse::stock404()
{
    HttpResponse resp;
    resp.status = 404;

    resp.statusText = "Not Found";

    resp.html("<h1>404 Not Found</h1>");
    return resp;
}

HttpResponse HttpResponse::stock416(size_t fileSize)
{
    HttpResponse resp;
    resp.status = 416;
    resp.statusText = "Range Not Satisfiable";
    resp.text("Range Not Satisfiable");
    resp.setHeader(
        "Content-Range",
        "bytes */" +
            std::to_string(fileSize));
    return resp;
}
void HttpResponse::reset()
{
    statusText="OK";
    status=200;
    headers.clear();
    body.reset();
    HeaderBody_.reset();
    keepAlive=true;
    chunked=false;
}

void HttpRequest::reset()
{
    valid=true;
    method.clear();
    raw_path.clear();
    path.clear();
    query.clear();
    version.clear();
    bodyData.clear();
    headers.clear();
    bodySize=0;
    querryParams.clear();
    range={};
}
