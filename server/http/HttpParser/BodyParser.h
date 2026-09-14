// ==============================================================================
// 文件名：BodyParser.h
// 职责比喻：这是"货物取出员"——专拆包裹最里层的"货物"（HTTP 正文）。
//   头部拆完后，本解析器负责取出正文。正文有两种"包装方式"：
//   - 固定长度（Content-Length）：标了多长就取多长，齐了才算完；
//   - 分块传输（chunked）：每块前面写十六进制长度，像拆俄罗斯套娃一块块取，直到 0 长度块结束。
//
// 关键技术点（初学者重点理解）：
//   1. 【固定长度必须整体到齐】Content-Length 标了 100 字节，Buffer 里只有 80 字节就返回
//      NEED_MORE，不消费任何数据，保证下次从同一起点继续，不丢正文开头。
//   2. 【chunked 三步走】分块体要经历 CHUNK_SIZE（读长度行）→CHUNK_DATA（读数据+CRLF）→
//      CHUNK_TRAILERS（读尾部字段）三个阶段，由 HttpParser 状态机驱动。
//   3. 【累计额度检查】headerBytes_ 同时累计普通头和 trailer，bodyBytes_ 累计所有 chunk，
//      防止攻击者借 trailer 或拆成多个小块绕过单次长度限制。
//   4. 【减法防溢出】用 "上限 - 已接收" 检查累计大小，避免加法溢出绕过 kMaxBodyBytes。
// ==============================================================================
#pragma once
#ifndef BODY_PARSER_H
#define BODY_PARSER_H

#include "../http.h"
#include "../../Buffer/Buffer.h"
#include "ParserUtils.h"

#include <limits>
#include <string>

// 负责解析 HTTP 正文：固定长度正文和 chunked 分块正文。
// contentLength_ / chunked_ / headerBytes_ 由 HttpParser 在首部解析完成后从 HeaderParser 传入；
// headerBytes_ 用于 trailer 的总额度检查，防止攻击者借 trailer 绕过首部长度限制。
/**
 * @brief 正文解析器——取出 HTTP 请求正文
 *
 * 通俗解释：拆完面单和说明标签后，最后取货物。货物要么"标了重量"（Content-Length）一次称齐，
 *   要么"分箱打包"（chunked）一箱箱拆。本解析员两种都会，按头部给的 framing 信息选对应方式。
 */
class BodyParser
{
public:
    // 处理固定长度正文
    ParseState parse(Buffer &buf, HttpRequest &req);
    // chunked：读取分块长度行
    ParseState parseChunkSize(Buffer &buf);
    // chunked：读取一块分块数据
    ParseState parseChunkData(Buffer &buf, HttpRequest &req);
    // chunked：读取尾部字段
    ParseState parseChunkTrailers(Buffer &buf);
    void reset();

    // 由 HttpParser 在首部解析完成后调用，把 HeaderParser 的结果传入。
    void setContentLength(size_t v) { contentLength_ = v; }
    void setChunked(bool v) { chunked_ = v; }
    void setHeaderBytes(size_t v) { headerBytes_ = v; }

    size_t contentLength() const { return contentLength_; }
    bool chunked() const { return chunked_; }
    size_t currentChunkSize() const { return currentChunkSize_; }
    size_t bodyBytes() const { return bodyBytes_; }
    size_t headerBytes() const { return headerBytes_; }

private:
    size_t contentLength_ = 0;       // Content-Length 值
    bool chunked_ = false;           // 是否 chunked 模式
    size_t currentChunkSize_ = 0;    // 当前块待读取的字节数
    // headerBytes_ 同时累计普通头和 trailer，bodyBytes_ 累计所有 chunk，
    // 防止攻击者通过拆成多行或多个小块绕过单次长度检查。
    size_t headerBytes_ = 0;         // 头部已累计字节（含 trailer）
    size_t bodyBytes_ = 0;           // 正文已累计字节
};

/**
 * @brief 解析固定长度正文
 * @param buf 连接读缓冲区
 * @param req 输出请求对象（bodyData/bodySize 填入）
 * @return PARSE_OK / PARSE_NEED_MORE / PARSE_ERROR
 *
 * 通俗解释：标了 100 字节就等够 100 字节再一次性取出。不够就原样放着等下次，保证下次从同一
 *   正文起点继续，不会丢开头。
 */
inline ParseState BodyParser::parse(Buffer &buf, HttpRequest &req)
{
    // 固定长度正文必须整体到齐后再写入请求对象；不足时不消费 Buffer，
    // 保证下一次读取可以从同一正文起点继续。
    if (contentLength_ > kMaxBodyBytes)
        return PARSE_ERROR;
    if (buf.readableBytes() < contentLength_)
        return PARSE_NEED_MORE;

    req.bodyData.assign(buf.peek(), contentLength_);
    bodyBytes_ = contentLength_;
    req.bodySize = contentLength_;
    // 清空缓存
    buf.retrieve(contentLength_);

    return PARSE_OK;
}

/**
 * @brief chunked：读取当前块的十六进制长度行
 * @param buf 连接读缓冲区
 * @return PARSE_OK / PARSE_NEED_MORE / PARSE_ERROR
 * @note chunk-size 为十六进制，可带扩展参数（;后内容），这里只接受扩展前的完整数字。
 *       0 长度块表示体结束，由 HttpParser 决定进入 trailer 还是数据读取。
 */
inline ParseState BodyParser::parseChunkSize(Buffer &buf)
{
    // chunk-size 为十六进制，可带扩展参数；这里只接受扩展前的完整数字。
    // 0 长度块不包含数据，状态直接进入 trailer，非零块则等待“数据 + CRLF”整体到齐。
    const char *lineEnd = buf.findCRLF();
    if (lineEnd == buf.beginWrite())
    {
        if (buf.readableBytes() > kMaxRequestLineBytes)
            return PARSE_ERROR;
        return PARSE_NEED_MORE;
    }
    if (static_cast<size_t>(lineEnd - buf.peek()) > kMaxRequestLineBytes)
        return PARSE_ERROR;
    std::string len(buf.peek(), lineEnd);
    try
    {
        // 支持 "1a;ext" 形式，只取分号前的数字部分
        const auto extension = len.find(';');
        const std::string sizeText = trim(len.substr(0, extension));
        if (sizeText.empty())
            return PARSE_ERROR;
        size_t parsed = 0;
        const auto parsedSize = std::stoull(sizeText, &parsed, 16); // 16 进制解析
        if (parsed != sizeText.size())
            return PARSE_ERROR;
        if (parsedSize > std::numeric_limits<size_t>::max())
            return PARSE_ERROR;
        currentChunkSize_ = static_cast<size_t>(parsedSize);
        // 防止后续 "+2"（CRLF）加法溢出
        if (currentChunkSize_ > SIZE_MAX - 2)
            return PARSE_ERROR;
    }
    catch (...)
    {
        return PARSE_ERROR;
    }
    // 消费长度行（含 CRLF），stage 转换由 HttpParser 状态机根据 currentChunkSize_ 决定。
    buf.retrieve((lineEnd - buf.peek()) + 2);
    return PARSE_OK;
}

/**
 * @brief chunked：读取当前块的数据 + 结尾 CRLF
 * @param buf 连接读缓冲区
 * @param req 输出请求对象（数据追加到 bodyData）
 * @return PARSE_OK / PARSE_NEED_MORE / PARSE_ERROR
 * @note 只有"数据 + CRLF"整体到齐才消费，非阻塞半包下不会丢掉 chunk 开头。
 */
inline ParseState BodyParser::parseChunkData(Buffer &buf, HttpRequest &req)
{
    // 使用“上限 - 已接收量”检查累计大小，避免加法溢出；只有数据及结尾 CRLF 都到齐才消费，
    // 从而在非阻塞半包下不会丢掉 chunk 的开头。
    if (bodyBytes_ > kMaxBodyBytes ||
        currentChunkSize_ > kMaxBodyBytes - bodyBytes_)
    {
        return PARSE_ERROR;
    }
    // 数据 + 结尾 \r\n 必须整体到齐
    if (buf.readableBytes() < currentChunkSize_ + 2)
    {
        return PARSE_NEED_MORE;
    }
    const char *chunkEnd = buf.peek() + currentChunkSize_;
    // 校验块结尾确实是 CRLF
    if (chunkEnd[0] != '\r' || chunkEnd[1] != '\n')
        return PARSE_ERROR;
    req.bodyData.append(buf.peek(), currentChunkSize_);
    bodyBytes_ += currentChunkSize_;
    req.bodySize = req.bodyData.size();
    // 消费数据 + CRLF，stage 转回 CHUNK_SIZE 由 HttpParser 状态机负责。
    buf.retrieve(currentChunkSize_ + 2);
    return PARSE_OK;
}

/**
 * @brief chunked：读取尾部字段（trailer），直到空行结束
 * @param buf 连接读缓冲区
 * @return PARSE_OK / PARSE_NEED_MORE / PARSE_ERROR
 * @note trailer 当前只校验基本字段形状（必须有冒号）而不暴露给业务层，但仍计入头部总额度，
 *       防止攻击者借 trailer 绕过头部限制。
 */
inline ParseState BodyParser::parseChunkTrailers(Buffer &buf)
{
    // trailer 当前只校验基本字段形状而不暴露给业务层，但仍计入头部总额度；
    // 这样未来扩展 trailer 存储时协议边界已正确，且不能借 trailer 绕过头部限制。
    while (true)
    {
        const char *lineEnd = buf.findCRLF();
        if (lineEnd == buf.beginWrite())
        {
            // 剩余额度检查，超限判错
            if (buf.readableBytes() > kMaxHeaderBytes - headerBytes_)
                return PARSE_ERROR;
            return PARSE_NEED_MORE;
        }
        const size_t lineBytes =
            static_cast<size_t>(lineEnd - buf.peek()) + 2;
        if (lineBytes > kMaxHeaderBytes - headerBytes_)
            return PARSE_ERROR;
        headerBytes_ += lineBytes;
        // 空行：trailer 结束
        if (lineEnd == buf.peek())
        {
            buf.retrieve(2);
            return PARSE_OK;
        }
        std::string trailer(buf.peek(), lineEnd);
        // trailer 行必须有冒号，否则非法
        if (trailer.find(':') == std::string::npos)
            return PARSE_ERROR;
        buf.retrieve((lineEnd - buf.peek()) + 2);
    }
}

/** @brief 重置正文解析器状态，准备下一个请求 */
inline void BodyParser::reset()
{
    contentLength_ = 0;
    chunked_ = false;
    currentChunkSize_ = 0;
    headerBytes_ = 0;
    bodyBytes_ = 0;
}

#endif
