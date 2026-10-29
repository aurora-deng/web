// ==============================================================================
// 文件名：HeaderParser.h
// 职责比喻：这是"内层包装拆解员"——专拆包裹的"内层说明标签"（HTTP 头部）。
//   请求行之后是一堆 "Key: Value" 形式的头部行，直到一个空行表示头部结束。本解析器逐行
//   拆解，把头部存进 req.headers，同时识别几个"决定请求边界"的关键头：
//   - Content-Length：正文多长（固定长度）；
//   - Transfer-Encoding: chunked：分块传输，正文边界靠块长度；
//   - Connection：keep-alive 还是 close；
//   - Host：HTTP/1.1 必须有；
//   - Range：断点续传范围。
//
// 关键技术点（初学者重点理解）：
//   1. 【framing 头自管】Content-Length / Transfer-Encoding / Connection / Range 这些头决定请求
//      边界，由 Parser 自己解析维护，不能只当普通键值丢给业务层——否则边界都确定不了。
//   2. 【chunked 与 Content-Length 互斥】两者并存会产生不同解析边界，引发请求走私，直接拒绝。
//   3. 【减法形式额度检查】用 "上限 - 已累计" 而非 "已累计 + 本行" 判断是否超长，避免 size_t
//      加法溢出绕过限制。
//   4. 【Host 必填】HTTP/1.1 要求且只允许一个 Host，缺失会让虚拟主机和代理路由歧义。
// ==============================================================================
#pragma once
#ifndef HEADER_PARSER_H
#define HEADER_PARSER_H

#include "../http.h"
#include "../../Buffer/Buffer.h"
#include "ParserUtils.h"

#include <limits>
#include <string>

// 负责解析 HTTP 首部，维护 framing 头（Content-Length / Transfer-Encoding / Connection / Range）。
// 这些头决定请求边界，由 Parser 自己维护，不能只作为普通键值交给业务层。
// keepAlive_ 初始值由 RequestLineParser 根据 HTTP 版本设置，Connection 头可覆盖。
/**
 * @brief 头部解析器——拆解 HTTP 请求头若干行
 *
 * 通俗解释：拆完快递面单（请求行）后，内层包装上贴着一张张"说明标签"（头部行）：
 *   "Content-Type: 货物类型"、"Content-Length: 货物重量"、"Connection: 是否继续合作"……
 *   本解析员逐张揭下来分类归档，并特别盯紧几个"决定怎么取货"的关键标签。
 */
class HeaderParser
{
public:
    /**
     * @brief 解析头部，直到遇到空行（头部结束标记）
     * @param buf 连接读缓冲区
     * @param req 输出请求对象（headers/range 等填入）
     * @return PARSE_OK（遇空行，头部完整）/ PARSE_NEED_MORE / PARSE_ERROR
     */
    ParseState parse(Buffer &buf, HttpRequest &req);

    /** @brief 重置解析器状态 */
    void reset();

    // 由 HttpParser 在请求行解析完成后调用，传入版本决定的默认值。
    void setKeepAlive(bool v) { keepAlive_ = v; }

    // ---- framing 状态 getter，供 HttpParser 传给 BodyParser ----
    size_t contentLength() const { return contentLength_; }
    bool keepAlive() const { return keepAlive_; }
    bool chunked() const { return chunked_; }
    bool hasContentLength() const { return hasContentLength_; }
    bool hasRange() const { return hasRange_; }
    const RangeInfo &range() const { return range_; }
    // 累计的首部字节数（不含请求行），BodyParser 解析 trailer 时在此基础上继续累计。
    size_t headerBytes() const { return headerBytes_; }

private:
    /**
     * @brief 解析 Range 头的值
     * @param s Range 头的值，如 "bytes=0-499" / "bytes=-500" / "bytes=1000-"
     * @param range 输出范围信息
     * @return true 合法；false 格式错误
     * @note 仅接受单个 bytes 范围，并区分闭区间、开放结尾和后缀长度三种形式。
     *       具体范围是否超出文件由响应层结合文件大小裁剪或拒绝，解析层只保证语法和数值安全。
     */
    bool parseRange(const std::string &s, RangeInfo &range);

    size_t contentLength_ = 0;   // Content-Length 值
    bool keepAlive_ = true;      // keep-alive 标志（版本默认 + Connection 头覆盖）
    bool hasRange_ = false;      // 是否有 Range 头
    RangeInfo range_;            // Range 范围信息
    // chunked 与 Content-Length 互斥，避免请求边界歧义带来的请求走私风险。
    bool chunked_ = false;
    bool hasContentLength_ = false; // 是否有 Content-Length 头
    bool hasHost_ = false;                  //用于和前端请求是否合法做判断
    size_t headerBytes_ = 0;     // 累计头部字节数（不含请求行）
};

/**
 * @brief 头部解析实现
 *
 * 通俗解释：循环"找一行→拆键值→归类存档→检查关键头"，直到遇到空行（头部结束）。
 *   每行都先检查是否超长，减法形式检查避免溢出绕过限制。
 */
inline ParseState HeaderParser::parse(Buffer &buf, HttpRequest &req)
{
    // 每确认一行就消费一行并累计字节；若当前行尚未完整则原样保留，等待下一次 recv。
    // 先做减法形式的剩余额度检查，可避免 size_t 加法溢出后绕过 kMaxHeaderBytes。
    while (true)
    {
        const char *lineEnd = buf.findCRLF();

        if (lineEnd == buf.beginWrite())
        {
            // 没找到 CRLF：剩余额度检查，超限判错，否则等下次
            if (buf.readableBytes() > kMaxHeaderBytes - headerBytes_)
                return PARSE_ERROR;
            return PARSE_NEED_MORE;
        }

        const size_t lineBytes =
            static_cast<size_t>(lineEnd - buf.peek()) + 2;
        // 减法形式额度检查，防溢出
        if (lineBytes > kMaxHeaderBytes - headerBytes_)
            return PARSE_ERROR;
        headerBytes_ += lineBytes;

        // 空行：头部结束标记
        if (lineEnd == buf.peek())
        {
            // HTTP/1.1 要求且只允许一个 Host；缺失 Host 会让虚拟主机和代理产生路由歧义。
            if (req.version == "HTTP/1.1" && !hasHost_)
                return PARSE_ERROR;
            buf.retrieve(2);

            return PARSE_OK;
        }

        std::string header(buf.peek(), lineEnd);

        auto pos = header.find(':');

        // 没有冒号不是合法头部行
        if (pos == std::string::npos)
            return PARSE_ERROR;

        std::string rawKey = header.substr(0, pos);
        // 字段名必须是合法 token，拒绝 "Content-Length :" 这种畸形写法
        if (!isHttpToken(rawKey))
            return PARSE_ERROR;
        std::string key = toLower(rawKey);

        std::string value = trim(header.substr(pos + 1));
        // 值中不允许出现控制字符（除制表符），防注入
        for (unsigned char c : value)
        {
            if ((c < 0x20 && c != '\t') || c == 0x7f)
                return PARSE_ERROR;
        }

        req.headers[key] = value;
        // ---- 识别关键 framing 头，分别处理 ----
        if (key == "host")
        {
            // Host 必须有且仅有一个，不能为空
            if (hasHost_ || value.empty())
                return PARSE_ERROR;
            hasHost_ = true;
        }
        else if (key == "content-length")
        {
            // 重复 Content-Length 或与 chunked 并存会产生不同解析边界，直接拒绝以消除歧义。
            if (chunked_ || hasContentLength_)
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
                contentLength_ = static_cast<size_t>(parsedLength);
                if (contentLength_ > kMaxBodyBytes)
                    return PARSE_ERROR;
                hasContentLength_ = true;
            }
            catch (...)
            {
                return PARSE_ERROR;
            }
        }

        else if (key == "connection")
        {
            // Connection 头可含多个逗号分隔的 token，逐个判断 keep-alive / close
            const auto v = toLower(value);
            bool sawKeepAlive = false;
            bool sawClose = false;
            size_t begin = 0;
            while (begin <= v.size())
            {
                const size_t comma = v.find(',', begin);
                const std::string token = trim(v.substr(
                    begin,
                    comma == std::string::npos
                        ? std::string::npos
                        : comma - begin));
                if (token == "close")
                    sawClose = true;
                else if (token == "keep-alive")
                    sawKeepAlive = true;
                if (comma == std::string::npos)
                    break;
                begin = comma + 1;
            }
            // close 具有最高优先级，避免组合字段被误判为可复用连接。
            if (sawClose)
                keepAlive_ = false;
            else if (sawKeepAlive)
                keepAlive_ = true;
        }
        else if (key == "transfer-encoding")
        {
            auto v = toLower(value);

            // 只接受 chunked；与 Content-Length 并存或重复 chunked 都判错
            if (v != "chunked" || hasContentLength_ || chunked_)
                return PARSE_ERROR;
            chunked_ = true;
        }
        else if (key == "range")
        {

            if (!parseRange(value, range_))
            {
                return PARSE_ERROR;
            }

            hasRange_ = true;
            req.range = range_;
        }
        buf.retrieve((lineEnd - buf.peek()) + 2);
    }
}

inline void HeaderParser::reset()
{
    contentLength_ = 0;
    keepAlive_ = true;
    hasRange_ = false;
    range_ = {};
    chunked_ = false;
    hasContentLength_ = false;
    hasHost_ = false;
    headerBytes_ = 0;
}

/**
 * @brief 解析 Range 头值
 * @param s Range 值，支持三种形式：
 *        "bytes=0-499"（闭区间）、"bytes=1000-"（开放结尾）、"bytes=-500"（后缀 500 字节）
 * @param range 输出范围信息
 * @return true 合法；false 格式错误
 *
 * 通俗解释：像拆"取货区间"——支持"从第 0 到 499 字节"、"从第 1000 字节到末尾"、"最后 500 字节"
 *   三种写法。解析层只管语法和数值合法，具体范围是否超文件由响应层裁剪。
 */
inline bool HeaderParser::parseRange(const std::string &s, RangeInfo &range)
{
    if (!s.starts_with("bytes="))
        return false;

    auto pos = s.find('-', 6);

    if (pos == std::string::npos)
    {
        return false;
    }
    // stuoll类型转换
    std::string left = s.substr(6, pos - 6);
    std::string right = s.substr(pos + 1);
    auto parseNumber = [](const std::string &text, size_t &value)
    {
        if (text.empty() ||
            text.find_first_not_of("0123456789") != std::string::npos)
        {
            return false;
        }
        try
        {
            const auto parsed = std::stoull(text);
            if (parsed > std::numeric_limits<size_t>::max())
                return false;
            value = static_cast<size_t>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    };

    range = {};
    range.enable = true;

    // ---- 三种形式：闭区间 / 开放结尾 / 后缀长度 ----
    if (!left.empty() && !right.empty())
    {
        // "bytes=0-499"：闭区间，begin 不能大于 end
        if (!parseNumber(left, range.begin) ||
            !parseNumber(right, range.end) ||
            range.begin > range.end)
            return false;
    }
    else if (!left.empty())
    {
        // "bytes=1000-"：从 begin 到文件末尾，end 用 SIZE_MAX 占位
        if (!parseNumber(left, range.begin))
            return false;
        range.end = SIZE_MAX;
    }
    else if (!right.empty())
    {
        // "bytes=-500"：最后 500 字节，suffix 模式
        range.suffix = true;
        if (!parseNumber(right, range.end) || range.end == 0)
            return false;
    }
    else
    {
        // "bytes=-"：两边都空，非法
        return false;
    }
    return true;
}

#endif
