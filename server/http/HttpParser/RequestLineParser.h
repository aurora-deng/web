#pragma once
#ifndef REQUEST_LINE_PARSER_H
#define REQUEST_LINE_PARSER_H

#include "../http.h"
#include "../../Buffer/Buffer.h"
#include "ParserUtils.h"

#include <string>
#include <unordered_map>

// 负责解析 HTTP 请求行（method raw_path version）及 query 参数。
// 请求行有自己的长度上限，半行数据保持原位等待下一次 recv；
// HTTP/1.1 默认 keep-alive，HTTP/1.0 默认 close，后续由 HeaderParser 根据 Connection 头覆盖。
class RequestLineParser
{
public:
    ParseState parse(Buffer &buf, HttpRequest &req);
    void reset();

    bool keepAlive() const { return keepAlive_; }
    // 请求行字节数（含 CRLF），仅作状态记录，供上层统计或调试使用。
    size_t headerBytes() const { return headerBytes_; }

private:
    // 按 & 和 = 切分 query，不执行百分号解码；保留简单、可预期的解析边界，
    // 后续若增加 URL 解码，应在明确处理非法编码和重复键策略后扩展。
    void parseQuery(const std::string &query, std::unordered_map<std::string, std::string> &params);

    bool keepAlive_ = true;
    size_t headerBytes_ = 0;
};

inline ParseState RequestLineParser::parse(Buffer &buf, HttpRequest &req)
{
    // 在 Buffer 上直接查找 CRLF，只在确认整行存在后复制请求行；
    // 半行数据保持原位，同时在找不到分隔符时执行长度上限检查，防止无限等待导致内存增长。
    const char *lineEnd = buf.findCRLF();

    if (lineEnd == buf.beginWrite())
    {
        if (buf.readableBytes() > kMaxRequestLineBytes)
            return PARSE_ERROR;
        return PARSE_NEED_MORE;
    }

    const size_t lineLength = static_cast<size_t>(lineEnd - buf.peek());
    if (lineLength > kMaxRequestLineBytes)
        return PARSE_ERROR;

    // 只复制第一行，后续全靠指针
    std::string request_line(buf.peek(), lineEnd);
    // 清空前后无用符号
    if (!request_line.empty() && request_line.back() == '\r')
        request_line.pop_back();

    const size_t firstSpace = request_line.find(' ');
    const size_t secondSpace =
        firstSpace == std::string::npos
            ? std::string::npos
            : request_line.find(' ', firstSpace + 1);
    if (firstSpace == 0 ||
        firstSpace == std::string::npos ||
        secondSpace == firstSpace + 1 ||
        secondSpace == std::string::npos ||
        request_line.find(' ', secondSpace + 1) != std::string::npos ||
        request_line.find('\t') != std::string::npos)
    {
        return PARSE_ERROR;
    }
    req.method = request_line.substr(0, firstSpace);
    req.raw_path = request_line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    req.version = request_line.substr(secondSpace + 1);
    if (!isHttpToken(req.method) || req.raw_path.empty())
        return PARSE_ERROR;
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0")
        return PARSE_ERROR;
    keepAlive_ = req.version == "HTTP/1.1";

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
    headerBytes_ = lineLength + 2;
    buf.retrieve(lineLength + 2);
    return PARSE_OK;
}

inline void RequestLineParser::reset()
{
    keepAlive_ = true;
    headerBytes_ = 0;
}

inline void RequestLineParser::parseQuery(const std::string &query, std::unordered_map<std::string, std::string> &params)
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

#endif
