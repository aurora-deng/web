// ==============================================================================
// 文件名：RequestLineParser.h
// 职责比喻：这是"快递面单拆解员"——专拆包裹最外层的"快递面单"（请求行）。
//   HTTP 请求第一行形如 "GET /api/user?id=123 HTTP/1.1"，本解析器把它拆成三段：
//   方法（GET）、原始路径（/api/user?id=123）、版本（HTTP/1.1），再顺手把查询串
//   （id=123）切分成参数表。这是状态机的第一个工位，拆完才能进入头部解析。
//
// 关键技术点（初学者重点理解）：
//   1. 【CRLF 定位整行】在 Buffer 上先找 \r\n，找到才复制整行；找不到说明行不完整，
//      返回 NEED_MORE 等下次，半行数据原样留在 Buffer 不动。
//   2. 【keepAlive 默认值】HTTP/1.1 默认 keep-alive，HTTP/1.0 默认 close。这个默认值
//      传给 HeaderParser，后续 Connection 头可覆盖。
//   3. 【path 与 query 分层】raw_path 保留原始 URL，path 去掉查询串供路由匹配，
//      query 单独解析成参数表。分层保存便于后续做 URL 解码而不丢原始信息。
//   4. 【严格空格校验】请求行必须有且仅有两个空格（method path version），多余空格或
//      制表符一律判错，避免歧义解析。
// ==============================================================================
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
// 解析请求请求头
/**
 * @brief 请求行解析器——拆解 HTTP 请求第一行
 *
 * 通俗解释：快递面单上有"收件方法、收件地址、单号版本"三栏，本解析员逐栏拆开填进 HttpRequest。
 *   如果面单没写完（半行），就先放着等写完再拆；如果写歪了（格式错）就拒收。
 */
class RequestLineParser
{
public:
    /**
     * @brief 解析请求行
     * @param buf 连接读缓冲区
     * @param req 输出请求对象（填 method/raw_path/path/query/version/querryParams）
     * @return PARSE_OK / PARSE_NEED_MORE / PARSE_ERROR
     */
    ParseState parse(Buffer &buf, HttpRequest &req);

    /** @brief 重置解析器状态，准备下一个请求 */
    void reset();

    /** @brief 返回解析得到的 keep-alive 默认值（基于 HTTP 版本） */
    bool keepAlive() const { return keepAlive_; }
    // 请求行字节数（含 CRLF），仅作状态记录，供上层统计或调试使用。
    size_t headerBytes() const { return headerBytes_; }

private:
    /**
     * @brief 把查询串切分成参数表
     * @param query 查询串（如 "id=123&name=abc"）
     * @param params 输出参数表
     * @note 按 & 和 = 切分，不执行百分号解码；保留简单、可预期的解析边界，
     *       后续若增加 URL 解码，应在明确处理非法编码和重复键策略后扩展。
     */
    void parseQuery(const std::string &query, std::unordered_map<std::string, std::string> &params);

    bool keepAlive_ = true;   // keep-alive 默认值，由 HTTP 版本决定
    size_t headerBytes_ = 0;  // 请求行字节数（含 CRLF）
};

/**
 * @brief 解析请求行实现
 *
 * 通俗解释：先在传送带上找面单的结尾（\r\n），找不到就等下一批货；找到了就把面单剪下来，
 *   按空格切成三栏，逐栏校验后填进档案。最后把地址栏里的"查询串"单独切出来存好。
 */
inline ParseState RequestLineParser::parse(Buffer &buf, HttpRequest &req)
{
    // 在 Buffer 上直接查找 CRLF，只在确认整行存在后复制请求行；
    // 半行数据保持原位，同时在找不到分隔符时执行长度上限检查，防止无限等待导致内存增长。
    const char *lineEnd = buf.findCRLF();

    if (lineEnd == buf.beginWrite())
    {
        // 没找到 CRLF：如果已累计数据超上限则判错，否则等下次数据
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

    // ---- 按空格切成 method / raw_path / version 三段，并严格校验空格数量 ----
    const size_t firstSpace = request_line.find(' ');
    const size_t secondSpace =
        firstSpace == std::string::npos
            ? std::string::npos
            : request_line.find(' ', firstSpace + 1);
    // 必须有且仅有两个空格：开头不能是空格、不能连续空格、不能有第三个空格、不能有制表符
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
    // 方法必须是合法 token，路径不能为空，版本只接受 1.0 / 1.1
    if (!isHttpToken(req.method) || req.raw_path.empty())
        return PARSE_ERROR;
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0")
        return PARSE_ERROR;
    // HTTP/1.1 默认 keep-alive，HTTP/1.0 默认 close（后续 Connection 头可覆盖）
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
    // 消费掉这一行（含 CRLF）
    buf.retrieve(lineLength + 2);
    return PARSE_OK;
}

inline void RequestLineParser::reset()
{
    keepAlive_ = true;
    headerBytes_ = 0;
}

/**
 * @brief 把查询串切分成参数表
 * @param query 查询串，如 "id=123&name=abc"
 * @param params 输出参数表
 *
 * 通俗解释：像拆"成对标签"——按 & 分成一组组，每组按 = 分成键值对。
 *   "id=123&name=abc" → {id:123, name:abc}。没有 = 的视为键值空串。
 */
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
            // 没有 = 的参数（如 ?flag），值为空串
            params[pair] = "";
        }

        start = end + 1;
    }
}

#endif
