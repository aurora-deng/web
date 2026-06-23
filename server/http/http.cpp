
#include "http.h"

// 作用：去掉空格
static inline std::string trim(const std::string &s)
{
    // 去掉前导空格
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
        start++;
    // 去掉后导空格
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r'))
        end--;
    return s.substr(start, end - start);
}

// 统一小写
static inline std::string toLower(std::string s)
{
    for (char &c : s)
        c = std::tolower((unsigned char)c);
    return s;
}
// 将query中信息拆分好放到map中
static void parseQuery(const std::string &query, std::unordered_map<std::string, std::string> &params)
{
    size_t start = 0;

    while (start < query.size())
    {
        size_t end = query.find('&', start);
        if (end == std::string::npos)
            end = query.size();

        std::string pair = query.substr(start, end - start);

        size_t eq = pair.find('=');

        if (eq != std::string::npos)
        {
            params[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
        else
        {
            params[pair] = "";
        }

        start = end + 1;
    }
}
static bool parseRange(const std::string &s, RangeInfo &range)
{
    if (!s.starts_with("bytes="))
        return false;

    auto pos = s.find('-');

    if (pos == std::string::npos)
    {
        return false;
    }
    // stuoll类型转换
    std::string left = s.substr(6, pos - 6);
    std::string right = s.substr(pos + 1);

    range.enable = true;

    if (!left.empty() && !right.empty())
    {
        range.begin = std::stoull(left);
        range.end = std::stoull(right);
        if (range.begin > range.end)
            return false;
    }
    else if (!left.empty())
    {
        range.begin = std::stoull(left);
        range.end = SIZE_MAX;
    }
    else if (!right.empty())
    {
        range.suffix = true;
        range.end = std::stoull(right);
    }
    else
    {
        return false;
    }
    return true;
}

// 零拷贝优化：通过使用指针扫描，实现只对部分数据进行复制，提高效率
ParseState try_parse_request(Buffer &buf, HttpRequest &req)
{
    // 读取一条信息
    // std::string_view data(buf.peek(),buf.readableBytes());
    // size_t header_end = data.find("\r\n\r\n");

    // 使用优化后的零拷贝,实现指针搜索
    const char *header_end = buf.findCRLFCRLF();
    if (!header_end)
        return PARSE_NEED_MORE;

    const char *body_start = header_end + 4;

    size_t header_len = header_end - buf.peek();

    // 使用指针搜索,找到第一行
    const char *line_end = std::search(buf.peek(), header_end, "\r\n", "\r\n" + 2);

    // 只复制第一行，后续全靠指针
    std::string request_line(buf.peek(), line_end);
    // 清空前后无用符号
    if (!request_line.empty() && request_line.back() == '\r')
        request_line.pop_back();

    std::istringstream iss(request_line);

    if (!(iss >> req.method >> req.raw_path >> req.version))
    {
        return PARSE_ERROR;
    }

    // 解析path和query
    // 将path拆分成path和query
    auto pos = req.raw_path.find('?');
    if (pos != std::string::npos)
    {
        req.query = req.raw_path.substr(pos + 1);
        req.path = req.raw_path.substr(0, pos);
        parseQuery(req.query, req.querryParams);
    }
    else
    {
        req.path = req.raw_path;
        req.query.clear();
    }

    // 优化使用零拷贝，减少重复isingstream复制的消耗
    const char *cur = line_end + 2;
    while (cur < header_end)
    {
        const char *next = std::search(cur, header_end, "\r\n", "\r\n" + 2);
        if (next == cur)
            break;

        std::string line(cur, next);

        auto pos = line.find(":");

        if (pos != std::string::npos)
        {
            std::string key = toLower(line.substr(0, pos));
            std::string value = trim(line.substr(pos + 1));
            req.headers[key] = value;
        }
        // 跳过\r\n
        cur = next + 2;
    }
    // 解析content—length
    size_t content_length = 0;
    auto length_local = req.headers.find("content-length");
    // 优化：防止stoul抛出异常直接终端程序，先做好预处理
    if (length_local != req.headers.end())
    {
        const std::string &cl = length_local->second;
        if (cl.empty() || cl.find_first_not_of("0123456789") != std::string::npos)
            return PARSE_ERROR;

        // 再次检查，用于日志显示,防止溢出
        try
        {
            unsigned long v = std::stoul(length_local->second);
            content_length = static_cast<size_t>(v);
        }
        catch (...) // ... 表示捕获所有类型的异常
        {
            return PARSE_ERROR;
        }
    }

    // 进行range检测
    auto tmp = req.headers.find("range");
    if (tmp != req.headers.end())
    {
        parseRange(tmp->second, req.range);
    }

    // header解析完成

    // 判断body是否完整
    if (buf.readableBytes() < header_len + 4 + content_length)
        return PARSE_NEED_MORE;

    req.bodyData = std::string(body_start, content_length);
    req.bodySize = content_length;
    // 清空缓存
    buf.retrieve(header_len + 4 + content_length);

    return PARSE_OK;
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
    headers["Cache-Control"] ="public,max-age=3600";

    constexpr size_t SMALL = KB(128);
    constexpr size_t MMAP = MB(16);

    // 小文件
    if (fileSize < SMALL)
    {
        auto buf = BufferPoll::instance().acquire();
        size_t len = sendEnd - sendBegin + 1;
        buf->ensureWrite(len);
        ssize_t n = pread(file->fd, buf->beginWrite(), fileSize, sendBegin);
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
        void *p = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, file->fd, 0);
        if (p != MAP_FAILED)
        {
            file->mmapPtr = p;
            file->mapped = true;
            b->use_mmap = true;
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
    if (status == 304)
    {
        res += "\r\n";
        return res;
    }
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
