#include "HttpParser.h"
#include <unicode/unistr.h>
static inline std::string toLower(std::string s);
static inline std::string trim(const std::string &s);
static void parseQuery(const std::string &query, std::unordered_map<std::string, std::string> &params);
static bool parseRange(const std::string &s, RangeInfo &range);
// 实现流式解析request
// 状态机持续推进，直到请求完成、需要更多字节或发现错误。
// TCP 没有消息边界，因此不能假设一次 recv 对应一次请求；这种增量设计同时覆盖半包与粘包。
ParseState HttpParser::parse(Buffer &buffer, HttpRequest &req)
{
    while (true)
    {
        switch (stage)
        {
        case ParseStage::REQUEST_LINE:
        {
            auto s = parseRequestLine(buffer, req);

            if (s != PARSE_OK)
                return s;

            stage = ParseStage::HEADERS;

            break;
        }
        case ParseStage::HEADERS:
        {
            auto s = parseHeaders(buffer, req);

            if (s != PARSE_OK)
                return s;

            if (chunked)
                stage = ParseStage::CHUNK_SIZE;
            else
                stage = ParseStage::BODY;

            break;
        }
        case ParseStage::BODY:
        {
            auto s = parseBody(buffer, req);

            if (s != PARSE_OK)
                return s;

            stage =
                ParseStage::COMPLETE;

            break;
        }
        case ParseStage::COMPLETE:

            return PARSE_OK;
        case ParseStage::CHUNK_SIZE:
        {

            auto s = parseChunkSize(buffer);

            if (s != PARSE_OK)
                return s;

            break;
        }

        case ParseStage::CHUNK_DATA:
        {

            auto s = parseChunkData(buffer, req);

            if (s != PARSE_OK)
                return s;

            break;
        }
        case ParseStage::CHUNK_TRAILERS:
        {
            auto s = parseChunkTrailers(buffer);
            if (s != PARSE_OK)
                return s;
            stage = ParseStage::COMPLETE;
            break;
        }
        default:

            return PARSE_ERROR;
        }
    }
}

void HttpParser::reset()
{
    stage = ParseStage::REQUEST_LINE;
    contentLength = 0;

    keepAlive_ = true;

    // chunked_=false;
}

ParseState HttpParser::parseRequestLine(Buffer &buf, HttpRequest &req)
{
    // 读取一条信息
    // std::string_view data(buf.peek(),buf.readableBytes());
    // size_t headerEnd = data.find("\r\n\r\n");
    // 只重置解析器自身，不清空 Buffer：其中可能已包含 keep-alive 连接的下一个请求，
    // 保留未消费字节可让 Session 下一轮直接解析，实现安全的连接复用。
    // 使用优化后的零拷贝,实现指针搜索
    const char *lineEnd = buf.findCRLF();

    if (lineEnd == buf.beginWrite())
    {
        if (buf.readableBytes() > MAX_REQUEST_LINE_BYTES)
            return PARSE_ERROR;
        return PARSE_NEED_MORE;
    }

    const size_t lineLength = static_cast<size_t>(lineEnd - buf.peek());
    if (lineLength > MAX_REQUEST_LINE_BYTES)
        return PARSE_ERROR;
    // 只复制第一行，后续全靠指针
    std::string request_line(buf.peek(), lineEnd);
    // 清空前后无用符号
    if (!request_line.empty() && request_line.back() == '\r')
        request_line.pop_back();

    std::istringstream iss(request_line);

    if (!(iss >> req.method >> req.raw_path >> req.version))
    {
        return PARSE_ERROR;
    }
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0")
        return PARSE_ERROR;
    keepAlive_ = req.version == "HTTP/1.1";

    // 解析path和query
    // 将path拆分成path和query
    // raw_path 保留原始目标，path 供路由匹配，query 单独解析到参数表；
    // 分层保存有利于后续加入 URL 解码或签名校验而不丢失原始表示。
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
    size_t len = lineEnd - buf.peek();

    buf.retrieve(len + 2);
    return PARSE_OK;
}

ParseState HttpParser::parseHeaders(Buffer &buf, HttpRequest &req)
{

    // 每确认一行就消费一行并累计字节；若当前行尚未完整则原样保留，等待下一次 recv。
    // 先做减法形式的剩余额度检查，可避免 size_t 加法溢出后绕过 MAX_HEADER_BYTES。
    while (true)
    {
        const char *lineEnd = buf.findCRLF();

        if (lineEnd == buf.beginWrite())
        {
            if (buf.readableBytes() > MAX_HEADER_BYTES - headerBytes)
                return PARSE_ERROR;
            return PARSE_NEED_MORE;
        }
        const size_t lineBytes = static_cast<size_t>(lineEnd - buf.peek()) + 2;
        if (lineBytes > MAX_HEADER_BYTES - headerBytes)
            return PARSE_ERROR;
        headerBytes += lineBytes;
        // 空行
        if (lineEnd == buf.peek())
        {
            buf.retrieve(2);

            return PARSE_OK;
        }

        std::string header(buf.peek(), lineEnd);

        auto pos = header.find(':');

        if (pos == std::string::npos)
            return PARSE_ERROR;

        std::string key = toLower(header.substr(0, pos));

        std::string value = trim(header.substr(pos + 1));

        req.headers[key] = value;
        //  Parser自己保存状态
        if (key == "content-length")
        {
            // 重复 Content-Length 或与 chunked 并存会产生不同解析边界，直接拒绝以消除歧义。
            if (chunked || hasContentLength)
                return PARSE_ERROR;
            if (value.empty() ||
                value.find_first_not_of("0123456789") != std::string::npos)
            {
                return PARSE_ERROR;
            }

            try
            {
                const auto parsedLength = std::stoull(value);
                if (parsedLength > std::numeric_limits<size_t>::max())
                    return PARSE_ERROR;
                contentLength = static_cast<size_t>(parsedLength);
                if (contentLength > MAX_BODY_BYTES)
                    return PARSE_ERROR;
                hasContentLength = true;
            }
            catch (...)
            {
                return PARSE_ERROR;
            }
        }

        else if (key == "connection")
        {

            auto v = toLower(value);

            if (v == "close")
            {
                keepAlive_ = false;
            }
            else if (v == "keep-alive")
            {
                keepAlive_ = true;
            }
        }
        else if (key == "transfer-encoding")
        {
            auto v = toLower(value);

            if (v == "chunked")
            {
                chunked = true;
            }
        }
        else if (key == "range")
        {

            if (!parseRange(value, range))
            {
                return PARSE_ERROR;
            }

            hasRange = true;
        }
        buf.retrieve((lineEnd - buf.peek()) + 2);
    }
}

ParseState HttpParser::parseChunkSize(Buffer &buf)
{
    // chunk-size 为十六进制，可带扩展参数；这里只接受扩展前的完整数字。
    // 0 长度块不包含数据，状态直接进入 trailer，非零块则等待“数据 + CRLF”整体到齐。
    const char *lineEnd = buf.findCRLF();
    if (lineEnd == buf.beginWrite())
        return PARSE_NEED_MORE;
    std::string len(buf.peek(), lineEnd);
    try
    {
        const auto extension = len.find(';');
        const std::string sizeText = trim(len.substr(0, extension));
        if (sizeText.empty())
            return PARSE_ERROR;
        size_t parsed = 0;
        const auto parsedSize = std::stoull(sizeText, &parsed, 16);
        if (parsed != sizeText.size())
            return PARSE_ERROR;
        if (parsedSize > std::numeric_limits<size_t>::max())
            return PARSE_ERROR;
        currentChunkSize = static_cast<size_t>(parsedSize);
        if (currentChunkSize > SIZE_MAX - 2)
            return PARSE_ERROR;
    }
    catch (...)
    {
        return PARSE_ERROR;
    }
    buf.retrieve((lineEnd - buf.peek()) + 2);
    if (currentChunkSize == 0)
    {
        stage = ParseStage::COMPLETE;
    }
    else
    {
        stage = ParseStage::CHUNK_DATA;
    }
    return PARSE_OK;
}

ParseState HttpParser::parseChunkData(Buffer &buf, HttpRequest &req)
{
    // 使用“上限 - 已接收量”检查累计大小，避免加法溢出；只有数据及结尾 CRLF 都到齐才消费，
    // 从而在非阻塞半包下不会丢掉 chunk 的开头。
    if (bodyBytes > MAX_BODY_BYTES ||
        currentChunkSize > MAX_BODY_BYTES - bodyBytes)
    {
        return PARSE_ERROR;
    }
    if (buf.readableBytes() < currentChunkSize + 2)
    {
        return PARSE_NEED_MORE;
    }
    const char *chunkEnd = buf.peek() + currentChunkSize;
    if (chunkEnd[0] != '\r' || chunkEnd[1] != '\n')
        return PARSE_ERROR;

    req.bodyData.append(buf.peek(), currentChunkSize);
    bodyBytes += currentChunkSize;
    req.bodySize = req.bodyData.size();
    buf.retrieve(currentChunkSize + 2);
    stage = ParseStage::CHUNK_SIZE;
    return PARSE_OK;
}

ParseState HttpParser::parseChunkTrailers(Buffer &buf)
{
    // trailer 当前只校验基本字段形状而不暴露给业务层，但仍计入头部总额度；
    // 这样未来扩展 trailer 存储时协议边界已正确，且不能借 trailer 绕过头部限制。
    while (true)
    {
        const char *lineEnd = buf.findCRLF();
        if (lineEnd == buf.beginWrite())
        {
            if (buf.readableBytes() > MAX_HEADER_BYTES - headerBytes)
                return PARSE_ERROR;
            return PARSE_NEED_MORE;
        }
        const size_t lineBytes =
            static_cast<size_t>(lineEnd - buf.peek()) + 2;
        if (lineBytes > MAX_HEADER_BYTES - headerBytes)
            return PARSE_ERROR;
        headerBytes += lineBytes;
        if (lineEnd == buf.peek())
        {
            buf.retrieve(2);
            return PARSE_OK;
        }
        std::string trailer(buf.peek(), lineEnd);
        if (trailer.find(':') == std::string::npos)
            return PARSE_ERROR;
        buf.retrieve((lineEnd - buf.peek()) + 2);
    }
}
ParseState HttpParser::parseBody(Buffer &buf, HttpRequest &req)
{

    // 判断body是否完整
    if (buf.readableBytes() < contentLength)
        return PARSE_NEED_MORE;

    req.bodyData.assign(buf.peek(), contentLength);
    req.bodySize = contentLength;
    // 清空缓存
    buf.retrieve(contentLength);

    return PARSE_OK;
}

// 统一小写
static inline std::string toLower(std::string s)
{
    for (char &c : s)
        c = std::tolower((unsigned char)c);
    return s;
}

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