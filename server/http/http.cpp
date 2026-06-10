
#include "http.h"
#include <mutex>
#include <fcntl.h>
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

    // 先解析http
    // std::string_view headerData = data.substr(0, header_end);

    // std::istringstream stream{std::string(headerData)};

    // std::string line;

    // // 解析第一行
    // if (!std::getline(stream,line))
    // {
    //     return PARSE_ERROR;
    // }

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

    // 解析headers
    // 解析header
    // while (std::getline(stream, line))
    // {
    //     if (line == "\r" && !line.empty())
    //         line.pop_back();

    //     if (line.empty())
    //         break;

    //     auto pos = line.find(':');
    //     if (pos == std::string::npos)
    //         continue;

    //     std::string key = toLower(line.substr(0, pos));
    //     std::string value = trim(line.substr(pos + 1));
    //     req.headers[key] = value;
    // }
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

    // 解析body
    // req.body.assign(body_start, content_length);
    // 使用零拷贝优化
    req.bodyData = body_start;
    req.bodySize = content_length;
    req.body = std::string_view(body_start, content_length);
    // 清空缓存
    buf.retrieve(header_len + 4 + content_length);

    return PARSE_OK;
}

// HttpRequest parse_request(const std::string& data)
// {
//     HttpRequest req;

//     std::istringstream stream(data);
//     std::string line;

//     // 解析第一行
//     std::getline(stream,line);
//     std::istringstream line_stream(line);

//     if(!(line_stream>>req.method>>req.path>>req.version))
//     {
//         req.valid=false;
//         return req;
//     }
//     // 将path拆分成path和query
//     auto pos=req.path.find('?');
//     if(pos!=std::string::npos)
//     {
//         req.query=req.path.substr(pos+1);
//         req.path=req.path.substr(0,pos);
//     }
//     // 解析header
//     while(std::getline(stream,line))
//     {
//         if(line=="\r"||line.empty())break;

//         auto pos=line.find(':');
//         if(pos==std::string::npos)continue;

//         std::string key=toLower(line.substr(0,pos));
//         std::string value=trim(line.substr(pos+1));
//         req.headers[key]=value;
//     }

//     // 解析body

//     size_t body_pos=data.find("\r\n\r\n");
//     if(body_pos!=std::string::npos)
//     {
//         req.body=data.substr(body_pos+4);
//     }
//     return req;
// }

HttpResponse::HttpResponse()
{
    body = std::make_shared<ResponseBody>();
}

// 既然使用sendfile就是在静态文件发送，可能range出现问题，但是还是静态文件，因此状态就要变成206
bool HttpResponse::sendfile(const std::string &path, RangeInfo &range)
{
    struct stat st;

    if (stat(path.c_str(), &st) == -1)
        return false;

    // // 整体作用：以只读方式打开磁盘文件，拿到文件句柄 fd，用于后续 sendfile 发送文件
    // int fd = open(path.c_str(), O_RDONLY);

    // if (fd == -1)
    //     return false;


    // 使用filecache优化处理open和close
    FileEntry file;
    if(!FileCache::instace().get(path,file))
    {
        
        return false;
    }
    filePath=path;
    filefd = file.fd;

    fileSize = file.size;
    if (!range.enable)
    {

        useSendfile = true;
        sendBegin = 0;
        sendEnd = fileSize - 1;
    }
    else
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
        useSendfile = true;
        // 更新Status
        status = 206;
        statusText = "Partial Content";
    }
    return true;
}

void HttpResponse::beginChunked()
{
    chunked = true;

    stream = std::make_shared<StreamQueue>();

    headers["Transfer-Encoding"] = "chunked";
}

void HttpResponse::writeChunk(const std::string &s)
{
    ChunkBolck c;
    std::stringstream ss;
    ss << std::hex << s.size();

    c.prefix = ss.str() + "\r\n";

    c.data = std::make_shared<ResponseBody>();

    c.data->data = s;

    c.suffix = "\r\n";

    stream->pushChunk(c);
}

void HttpResponse::endChunked()
{
    std::lock_guard lock(stream->mtx);
    stream->finished = true;
}

// std::string HttpResponse::toString() const
// {
//     std::string res;

//     res +=
//         "HTTP/1.1 " + std::to_string(status) +
//         " " +
//         statusText +
//         "\r\n";
//     // 优化使用chunk
//     // bool hasCL = false;
//     for (auto &[k, v] : headers)
//     {
//         std::string lk=toLower(k);
//         if (lk== "content-length"||lk=="transfer-encoding")
//         {
//             continue;
//         }

//         res += k + ": " + v + "\r\n";
//         //     hasCL = true;
//     }

//     // if (!hasCL)

//     if (keepAlive)
//     {
//         res += "Connection: keep-alive\r\n";
//     }
//     else
//     {
//         res += "Connection: close\r\n";
//     }

//     // chunked标记
//     if(chunked)
//     {
//         res+="Transfer-Encoding: chunked\r\n";
//     }else{
//         res += "Content-Length: " + std::to_string(body->data.size()) + "\r\n";
//     }

//     // 头部结束
//     res+="\r\n";
//     if(chunked)
//     {
//         for(auto &data:chunks)
//         {
//             std::stringstream ss;
//             ss<<std::hex<<data.size();

//             res+=ss.str();
//             res+="\r\n";

//             res+=data;
//             res+="\r\n";
//         }

//         res+="0\r\n\r\n";
//     }else{
//         res += body->data;
//     }
//     return res;
// }

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
    else if (useSendfile)
    {
        if (useSendfile)
        {
            res += "Accept-Ranges: bytes\r\n";
        }
        if (status == 206)
        {
            res += "Content-Range: bytes ";
            res += std::to_string(sendBegin);
            res += "-";
            res += std::to_string(sendEnd);
            res += "/";
            res += std::to_string(fileSize);
            res += "\r\n";

            res += "Content-Length: ";
            res += std::to_string(sendEnd - sendBegin + 1);
            res += "\r\n";
        }
        else if (status == 416)
        {
            res += "Content-Range: bytes ";
            res += "*/" + std::to_string(fileSize) + "\r\n";
        }
        else
        {
            res += "Content-Length: " + std::to_string(fileSize) + "\r\n";
        }
    }
    else
    {
        res += "Content-Length: " + std::to_string(body->data.size()) + "\r\n";
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
    body->data = s;
    headers["Content-Type"] = "text/plain";
}

void HttpResponse::html(const std::string &s)
{
    body->data = s;
    headers["Content-Type"] = "text/html";
}

void HttpResponse::json(const std::string &s)
{
    body->data = s;
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

void StreamQueue::pushChunk(ChunkBolck c)
{
    {
        std::lock_guard lock(mtx);

        chunks.push_back(std::move(c));
    }
    if (wakeup)
        wakeup();
}

FileCache &FileCache::instace()
{
    static FileCache filecache;
    return filecache;
}

bool FileCache::get(const std::string &path, FileEntry &out)
{
    std::lock_guard lock(mtx);

    auto it=cache.find(path);

    if(it!=cache.end())
    {
        it->second.refCount++;
        out=it->second;
        return true;
    }
    int fd=open(path.c_str(),O_RDONLY);

    if(fd<0)return false;

    struct stat st;
    if(fstat(fd,&st)<0)
    {
        close(fd);
        return false;
    }

    FileEntry entry;
    entry.size=st.st_size;
    entry.mtime=st.st_mtime;
    entry.fd=fd;
    entry.refCount=1;


    cache[path]=entry;
    out=entry;
    return true;
}

void FileCache::put(const std::string &path)
{
    std::lock_guard lock(mtx);

    auto it=cache.find(path);

    if(it==cache.end())return;

    it->second.refCount--;

    if(it->second.refCount<=0)
    {
        close(it->second.fd);
        cache.erase(it);
    }
}

FileCache::~FileCache()
{
    for(auto&[k,v]:cache)
    {
        close(v.fd);
    }
}
