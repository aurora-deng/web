// ==============================================================================
// 文件名：http.cpp
// 职责比喻：这是 HttpResponse 结构的"装配车间"。
//   http.h 只画了响应信封的"图纸"（字段定义），本文件是真正动手"造信封"的车间：
//   - 把磁盘文件装进响应体（sendfile，支持零拷贝和断点续传）；
//   - 把响应头拼接成字节流（buildHeader）；
//   - 提供分块传输（chunked）的流式写入能力；
//   - 提供便捷的 text/html/json 响应构造方法；
//   - 生成标准错误响应（404/416）。
//   整个车间围绕"如何把响应内容高效装进 HttpResponse"展开，重点在性能（零拷贝、对象池复用）。
//
// 第四阶段为支持 WebSocket 做的改造（均在 buildHeader() 中）：
//   - 新增 hasConnection 检测：遍历业务头时记录是否已设 Connection 头，避免重复追加
//     keep-alive/close 覆盖 WebSocket 升级的 Upgrade 语义。
//   - 新增 status==101 提前返回分支：101 Switching Protocols 响应不能带 Content-Length /
//     Transfer-Encoding（RFC 6455 规定握手响应无响应体），写完头部空行后直接返回。
//
// 关键技术点（初学者重点理解）：
//   1. 【FileCache 文件缓存】磁盘文件由 FileCache 统一管理 fd/mmap/etag，避免每次请求都
//      open/close，热文件直接复用 mmap 映射，省去内核到用户态的内存拷贝。
//   2. 【零拷贝三档策略】按文件大小分档：<128KB 用 pread 进缓冲区；<16MB 用 mmap 映射；
//      更大或未映射则走 FileBody 的 sendfile 路径。小、中、大文件各取最优。
//   3. 【304 缓存协商】客户端带上 If-None-Match，服务器比对 ETag，未变就回 304 空响应体，
//      让浏览器用本地缓存，省带宽省时间。
//   4. 【对象池 BufferPoll】所有临时缓冲区从对象池借出，用完归还，避免频繁 malloc/free。
// ==============================================================================
#include "http.h"
// 统一小写：HTTP 头部名大小写不敏感，统一转小写后存储和比较，避免 "Content-Type" 与
// "content-type" 被当成两个不同头。
static inline std::string toLower(std::string s)
{
    for (char &c : s)
        c = std::tolower((unsigned char)c);
    return s;
}

HttpResponse::HttpResponse()
{
}

/**
 * @brief 把时间戳格式化为 RFC 1123 格式的 HTTP 日期字符串
 * @param t Unix 时间戳（1970-01-01 UTC 起的秒数）
 * @return 形如 "Wed, 04 Aug 2026 12:00:00 GMT" 的字符串
 * @note HTTP 协议要求日期头用 GMT 零时区，这样全球客户端解析结果一致，不会有时区歧义。
 */
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

/**
 * @brief 根据文件大小和修改时间生成 ETag（实体标签）
 * @param size 文件字节数
 * @param mtime 文件最后修改时间
 * @return 形如 "12345-1691234567" 的带引号字符串
 * @note ETag 是文件的"指纹"，客户端下次请求带上 If-None-Match，服务器比对即可判断文件
 *       有没有变过，实现 304 缓存协商。用 size+mtime 简单可靠，无需读文件内容算哈希。
 */
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
/**
 * @brief 把磁盘文件装入响应体（静态文件响应入口）
 * @param path 文件磁盘路径
 * @param req  对应请求（用于读取 If-None-Match 做缓存协商）
 * @param range 客户端请求的字节范围
 * @return true 装入成功；false 文件不存在或读取失败
 *
 * 通俗解释：这是"文件快递打包台"。客户端想下载一个文件，服务器在这里把文件"装车"。
 *   会先查 FileCache 看文件是否已缓存（省去重复 open），再按大小选最优发送方式：
 *   小文件读进内存、中文件 mmap 映射、大文件走 sendfile 零拷贝。
 *
 * 【缓存协商 通俗解释】客户端之前下过这个文件，本地存了 ETag。再次请求时带 If-None-Match，
 *   服务器比对：标签一样说明文件没变，回 304 让客户端用本地缓存；不一样才回新内容。
 *
 * 【206 部分内容 通俗解释】客户端用 Range 头只要文件的一段（如视频拖进度条），服务器回 206
 *   并在 Content-Range 头里说明这次给了哪一段。范围越界则回 416。
 */
bool HttpResponse::sendfile(const std::string &path, const HttpRequest &req, RangeInfo &range)
{
    struct stat st;

    if (stat(path.c_str(), &st) == -1)
        return false;

    // // 整体作用：以只读方式打开磁盘文件，拿到文件句柄 fd，用于后续 sendfile 发送文件
    // int fd = open(path.c_str(), O_RDONLY);

    // if (fd == -1)
    //     return false;

    // 使用filecache优化处理open和close：FileCache 统一管理文件 fd 和 mmap，热路径免 open/close
    FileEntryPtr file = FileCache::instace().get(path);
    if (!file)
    {
        return false;
    }
    // 判断是否复用：客户端带了 If-None-Match 且与文件 ETag 一致，文件未变，回 304 走缓存
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

    // ---- 处理 Range 断点续传：把客户端请求的范围换算成实际发送的 [begin, end] ----
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
        // 越界处理：请求范围超出文件大小，回 416 告诉客户端文件实际多大
        if (sendBegin > fileSize || sendBegin > sendEnd)
        {
            *this = stock416(fileSize);
            return true;
        }
        // 更新Status：有 Range 且范围合法，状态变为 206 Partial Content
        status = 206;
        statusText = "Partial Content";
        headers["Accept-Ranges"] = "bytes";
    }
    headers["ETag"] = file->etag;

    headers["Last-Modified"] = file->lastModified;
    headers["Cache-Control"] = "public,max-age=3600";

    // ---- 按文件大小分档选择发送策略：小文件进内存，中大文件走 FileBody ----
    constexpr size_t SMALL = KiB(128);
    constexpr size_t MMAP = MiB(16);

    // 小文件：<128KB 直接 pread 进对象池缓冲区，构造 StringBody，简单高效
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
    // 大文件：<16MB 尝试 mmap 映射，把文件"贴"到进程地址空间，避免内核→用户态拷贝
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

/**
 * @brief 开启分块传输模式（chunked Transfer-Encoding）
 * @note 当响应体大小未知或需要边产生边发送时使用，如流式响应、SSE。
 *       每个 chunk 前面写十六进制长度行，最后写 0 长度块结束。
 */
void HttpResponse::beginChunked()
{
    chunked = true;

    body = std::make_shared<ChunkedBody>();
    headers["Transfer-Encoding"] = "chunked";
}

/**
 * @brief 追加一个分块到 chunked 响应体
 * @param s 这一帧的数据内容
 * @note 自动生成 "十六进制长度\r\n" + 数据 + "\r\n" 的 chunk 格式，业务侧无需关心协议细节。
 */
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

/**
 * @brief 结束 chunked 流，写入终止标记
 * @note 调用 body->finish() 写入 "0\r\n\r\n" 终止块，告诉客户端响应体结束。
 */
void HttpResponse::endChunked()
{
    if (body)
        body->finish();
}

/**
 * @brief 拼接响应头为字符串（const 版本，旧路径/调试用）
 * @return 完整响应头字符串（含末尾空行 \r\n）
 * @note 生产路径用下面的 void buildHeader() 把头写进 HeaderBody_，避免 string 拼接拷贝。
 *       本函数会跳过业务已设置的 Content-Length / Transfer-Encoding，由框架统一计算。
 */
std::string HttpResponse::buildHeader() const
{
    std::string res;
   
    // ---- 状态行：HTTP/1.1 200 OK ----
    res +=
        "HTTP/1.1 " + std::to_string(status) +
        " " +
        statusText +
        "\r\n";
    // 优化使用chunk
    // bool hasCL = false;
    // 遍历业务自定义头：Content-Length / Transfer-Encoding 跳过，由下面统一处理，
    // 避免业务手填的值与实际响应体不符导致响应边界错乱。
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

    // ---- Connection 头：Keep-Alive 还是 close ----
    if (keepAlive)
    {
        res += "Connection: keep-alive\r\n";
    }
    else
    {
        res += "Connection: close\r\n";
    }
    // ---- chunked 标记：分块传输则写 Transfer-Encoding，否则写 Content-Length ----
    if (chunked)
    {
        res += "Transfer-Encoding: chunked\r\n";
    }
    else
    {
        auto file = std::dynamic_pointer_cast<FileBody>(body);

        if (file)
        {
            // 文件响应：声明支持断点续传，并按状态写 Content-Range / Content-Length
            res += "Accept-Ranges: bytes\r\n";

            if (status == 206)
            {
                // 部分内容：写明本次给了哪个字节范围
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
                // 范围越界：用 */size 表示"整个文件就这么多"
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
            // 内存响应体：直接用 body 的内存占用作为 Content-Length
            size_t memSize = body ? body->memoryUsage() : 0;
            res += "Content-Length: " + std::to_string(memSize) + "\r\n";
        }
    }

    // 头部结束：空行 \r\n 标识头部与正文的分界
    res += "\r\n";
    return res;
}

/**
 * @brief 序列化响应头进 HeaderBody_（生产路径，零拷贝友好）
 * @note 与 const 版本逻辑相同，但写入对象池借出的 Buffer，再包成 HeaderBody，
 *       供 TransportWriter 在 writerLoop 内用 writev 聚集发送，避免大字符串拷贝。
 *
 * 【101 Switching Protocols 通俗解释】WebSocket 升级握手回 101，此时没有响应体，
 *   必须跳过 Content-Length / chunked，否则客户端会误等一个不存在的体。
 */
void HttpResponse::buildHeader()
{
    auto buf=BufferPoll::instance().acquire();
   
    // ---- 状态行 ----
    buf->append
        ("HTTP/1.1 " + std::to_string(status) +
        " " +
        statusText +
        "\r\n");
    // 第四阶段新增 hasConnection 检测：遍历业务头时记录是否已设 Connection 头。
    // 若业务已显式设置 Connection（例如 WebSocket 升级的 Upgrade: websocket + Connection: Upgrade），
    // 不再追加 keep-alive/close，避免重复或覆盖 Upgrade 语义。
    bool hasConnection = false;
    for (auto &[k, v] : headers)
    {
        std::string lk = toLower(k);
        if (lk == "content-length" || lk == "transfer-encoding")
        {
            continue;
        }
        if (lk == "connection")
            hasConnection = true;

        buf->append(k + ": " + v + "\r\n");
    }

    // 业务没设 Connection 时，按 keepAlive 标志补默认值
    if (!hasConnection)
    {
        if (keepAlive)
        {
            buf->append("Connection: keep-alive\r\n");
        }
        else
        {
            buf->append("Connection: close\r\n");
        }
    }

    // 第四阶段新增：status==101 提前返回分支。
    // RFC 6455 规定 WebSocket 握手响应（101 Switching Protocols）不能带响应体，
    // 因此必须跳过 Content-Length / Transfer-Encoding，只写头部空行后直接返回。
    // 否则客户端会误等一个不存在的响应体，导致握手失败。
    if (status == 101)
    {
        buf->append("\r\n");
        HeaderBody_ = std::make_shared<HeaderBody>(buf);
        return;
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

    // 头部结束：空行分界
    buf->append ("\r\n");
    HeaderBody_=std::make_shared<HeaderBody>(buf);
}

void HttpResponse::setHeader(const std::string key, std::string value)
{
    headers[key] = std::move(value);
}

// ---- 便捷响应构造：text/html/json，从对象池借缓冲区，避免临时 string 拷贝 ----
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

/**
 * @brief 工厂方法：生成标准 404 响应
 * @return 装好 404 状态和 HTML 体的 HttpResponse 对象
 */
HttpResponse HttpResponse::stock404()
{
    HttpResponse resp;
    resp.status = 404;

    resp.statusText = "Not Found";

    resp.html("<h1>404 Not Found</h1>");
    return resp;
}

/**
 * @brief 工厂方法：生成 416 范围越界响应
 * @param fileSize 文件实际大小，写入 Content-Range 告诉客户端合法范围
 * @return 装好 416 状态和 Content-Range 头的 HttpResponse 对象
 */
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

/**
 * @brief 重置响应为初始状态，供对象池复用
 * @note 复用连接处理下一个请求前必须 reset，避免上一个请求的残留字段污染新响应。
 */
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

/**
 * @brief 重置请求为初始状态，供连接复用解析下一个请求
 * @note range 用 {} 重置为默认值（enable=false 等），保证下一个请求不会被上一个 Range 影响。
 */
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
