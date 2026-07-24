#include "HttpParser.h"
#include <unicode/unistr.h>

// 实现流式解析request
ParseState HttpParser::parse(Buffer &buffer, HttpRequest &req)
{
    while (true)
    {
        switch (stage)
        {
        case ParseStage::REQUEST_LINE:
        {
            auto s =
                parseRequestLine(buffer, req);

            if (s != PARSE_OK)
                return s;

            stage =
                ParseStage::HEADERS;

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
            auto s =
                parseBody(buffer, req);

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

    // 使用优化后的零拷贝,实现指针搜索
    const char *lineEnd = buf.findCRLF();

    if (lineEnd == buf.beginWrite())
        return PARSE_NEED_MORE;

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
    size_t len = lineEnd - buf.peek();

    buf.retrieve(len + 2);
    return PARSE_OK;
}

ParseState HttpParser::parseHeaders(Buffer &buf, HttpRequest &req)
{

    while (true)
    {
        const char *lineEnd = buf.findCRLF();

        if (lineEnd == buf.beginWrite())
            return PARSE_NEED_MORE;

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
            if (value.empty() ||
                value.find_first_not_of("0123456789") != std::string::npos)
            {
                return PARSE_ERROR;
            }

            try
            {
                contentLength = std::stoull(value);
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
    const char *lineEnd = buf.findCRLF();
    if (lineEnd == buf.beginWrite())
        return PARSE_NEED_MORE;
    std::string len(buf.peek(), lineEnd);
    try
    {
        currentChunkSize = std::stoul(len, nullptr, 16);
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
    if (buf.readableBytes() < currentChunkSize + 2)
    {
        return PARSE_NEED_MORE;
    }
    req.bodyData.append(buf.peek(), currentChunkSize);
    buf.retrieve(currentChunkSize + 2);
    stage = ParseStage::CHUNK_SIZE;
    return PARSE_OK;
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