/**
 * @file unit_tests.cpp
 * @brief 质量回归单元测试：直接覆盖缓冲区、HTTP 解析、字节范围和路由等基础组件。
 *
 * 这些测试刻意使用小而确定的输入定位协议边界，既为框架重构提供快速反馈，
 * 也避免只依赖端到端测试时难以区分解析、响应构造和路由层故障。
 *
 * 测试框架：Google Test (gtest)
 * 运行方式：ctest --test-dir build-tests --output-on-failure
 *
 * 测试覆盖：
 *   - Buffer          : 追加、压缩、分隔符查找
 *   - HttpParser      : 半包/粘包/chunked/range/长度限制/请求走私/reset
 *   - ResponseSender  : 独立于 SubReactor 的发送验证
 *   - HttpRange       : 闭区间/开区间/后缀范围/Content-Range
 *   - Router          : 动态路由/中间件短路/404
 */

#include <gtest/gtest.h>

#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "server/Buffer/Buffer.h"
#include "server/Repsonse/FileBody.h"
#include "server/Repsonse/StringBody.h"
#include "server/Route/Router.h"
#include "server/http/HttpParser/HttpParser.h"
#include "server/http/ResponseSender/ResponseSender.h"

namespace {

/**
 * @brief 从响应体中提取实际字节内容的辅助函数。
 *
 * 通过 dynamic_pointer_cast 同时验证路由确实选择了 StringBody；
 * 空指针返回空串，避免辅助函数掩盖崩溃风险（断言仍会失败）。
 *
 * @param response 待检查的 HTTP 响应对象
 * @return 响应体的字符串内容；若类型不匹配或缓冲区为空则返回空串
 */
std::string responseBody(const HttpResponse& response)
{
    auto body = std::dynamic_pointer_cast<StringBody>(response.body);
    if (!body || !body->buffer_)
        return {};
    return std::string(body->buffer_->peek(), body->buffer_->readableBytes());
}

// ============================================================================
// Buffer 测试
// ============================================================================

/**
 * @test BufferTest.AppendsCompactsAndFindsDelimiters
 * @brief 验证 Buffer 的追加、压缩和分隔符查找功能。
 *
 * 测试步骤：
 *   1. 创建初始容量为 8 的小 Buffer，迫使后续操作触发压缩或扩容
 *   2. 追加 "abcd" 4 字节，消费前 3 字节，再追加 6 字节
 *   3. 验证可读字节数为 7，内容为 "defghij"（消费+追加后的数据搬移正确）
 *   4. 清空后追加 "one\r\ntwo\r\n\r\n"，验证 findCRLF 和 findCRLFCRLF
 *
 * 为什么重要：
 *   - Buffer 是增量解析的基础，数据搬移后可读区必须连续且内容不丢失
 *   - 分隔符查找（CRLF、CRLFCRLF）是 HTTP 协议解析的核心依赖
 *   - 此测试覆盖分包场景下追加数据的安全性
 */
TEST(BufferTest, AppendsCompactsAndFindsDelimiters)
{
    // 小初始容量会迫使 Buffer 在已消费前缀存在时压缩或扩容，验证数据搬移后
    // 可读区仍连续且内容不丢失，这是分包解析能够安全追加数据的基础。
    Buffer buffer(8);
    buffer.append("abcd", 4);
    buffer.retrieve(3);
    buffer.append("efghij", 6);

    EXPECT_EQ(buffer.readableBytes(), 7U);
    EXPECT_EQ(std::string(buffer.peek(), buffer.readableBytes()), "defghij");

    buffer.clear();
    // 同时检查行结束符和首部结束符的相对位置，防止查找函数越过可读边界，
    // 或在多段协议分隔符中返回错误位置。
    buffer.append("one\r\ntwo\r\n\r\n", 12);
    ASSERT_NE(buffer.findCRLF(), buffer.beginWrite());
    ASSERT_NE(buffer.findCRLFCRLF(), nullptr);
    EXPECT_EQ(buffer.findCRLF() - buffer.peek(), 3);
    EXPECT_EQ(buffer.findCRLFCRLF() - buffer.peek(), 8);
}

// ============================================================================
// HttpParser 测试
// ============================================================================

/**
 * @test HttpParserTest.WaitsForHalfPacketAndContentLengthBody
 * @brief 验证解析器对 TCP 半包 + Content-Length 正文的处理。
 *
 * 测试步骤：
 *   1. 首次只发送部分请求体 ("...Content-Length: 5\r\n\r\nhe")
 *   2. 期望解析器返回 PARSE_NEED_MORE（数据不够，不能提前成功）
 *   3. 追加剩余正文 "llo"
 *   4. 期望解析器返回 PARSE_OK，且 bodyData 为 "hello"
 *
 * 为什么重要：
 *   - TCP 半包是事件驱动服务器最常见的输入形态
 *   - 解析器必须保持跨多次调用的状态上下文
 *   - Content-Length 正文必须完整组装后才能交付
 */
TEST(HttpParserTest, WaitsForHalfPacketAndContentLengthBody)
{
    HttpParser parser;
    HttpRequest request;
    Buffer buffer;

    // 首次只到达部分请求体，应保持解析上下文而不是提前成功；补齐后必须
    // 正确组装正文。该场景覆盖 TCP 半包，是事件驱动服务器最常见的输入形态。
    buffer.append("POST /submit HTTP/1.1\r\nHost: test\r\nContent-Length: 5\r\n\r\nhe");
    EXPECT_EQ(parser.parse(buffer, request), PARSE_NEED_MORE);

    buffer.append("llo");
    EXPECT_EQ(parser.parse(buffer, request), PARSE_OK);
    EXPECT_EQ(request.method, "POST");
    EXPECT_EQ(request.path, "/submit");
    EXPECT_EQ(request.bodyData, "hello");
    EXPECT_EQ(request.bodySize, 5U);
    EXPECT_TRUE(parser.keepAlive());
}

/**
 * @test HttpParserTest.LeavesPipelinedRequestInBuffer
 * @brief 验证解析器对 HTTP 流水线（粘包）的处理。
 *
 * 测试步骤：
 *   1. 一次读入两个完整请求（GET /first 和 GET /second）
 *   2. 期望解析器只消费第一个请求的字节
 *   3. 验证 buffer 中仍有剩余数据（第二个请求）
 *   4. reset 后再次解析，成功消费第二个请求
 *   5. 验证 keep-alive 状态不会从前一请求泄漏
 *
 * 为什么重要：
 *   - HTTP 流水线（pipelining）要求一次读入的多个请求能被逐个处理
 *   - 解析器必须精确消费已解析字节，不丢失后续请求
 *   - parser.reset() 必须清除前一请求的状态，防止串读
 */
TEST(HttpParserTest, LeavesPipelinedRequestInBuffer)
{
    HttpParser parser;
    HttpRequest first;
    Buffer buffer;
    // 一次读入两个请求，解析器只能消费第一个请求的字节，并将后一个请求
    // 留给 reset 后继续解析；否则 HTTP 流水线会出现请求丢失或粘包串读。
    buffer.append(
        "GET /first HTTP/1.1\r\nHost: test\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n");

    ASSERT_EQ(parser.parse(buffer, first), PARSE_OK);
    EXPECT_EQ(first.path, "/first");
    EXPECT_GT(buffer.readableBytes(), 0U);

    // 第二个请求显式关闭连接，顺带验证 keep-alive 状态不会从前一请求泄漏。
    parser.reset();
    HttpRequest second;
    ASSERT_EQ(parser.parse(buffer, second), PARSE_OK);
    EXPECT_EQ(second.path, "/second");
    EXPECT_FALSE(parser.keepAlive());
    EXPECT_EQ(buffer.readableBytes(), 0U);
}

/**
 * @test HttpParserTest.DecodesChunkedBodyAcrossPackets
 * @brief 验证解析器对 chunked 编码正文跨包解析的处理。
 *
 * 测试步骤：
 *   1. 第一次发送：chunked 请求头 + 第一个 chunk 的部分数据 ("4\r\nWi")
 *   2. 期望返回 PARSE_NEED_MORE
 *   3. 第二次发送：剩余 chunk 数据 + 第二个 chunk + 终止块 + trailer
 *   4. 期望解析成功，bodyData 为 "Wikipedia"（拼接两个 chunk 的结果）
 *
 * 为什么重要：
 *   - chunked 编码是 HTTP 流式传输的核心格式
 *   - chunk 长度、剩余字节数等状态必须跨 parse 调用保存
 *   - trailer（chunk 后附加首部）必须完整消费
 *   - 为流式上传和代理转发提供稳定解析基础
 */
TEST(HttpParserTest, DecodesChunkedBodyAcrossPackets)
{
    HttpParser parser;
    HttpRequest request;
    Buffer buffer;
    // 在 chunk 数据中间断开输入，验证 chunk 长度、剩余字节数等状态可跨调用保存。
    buffer.append(
        "POST /upload HTTP/1.1\r\n"
        "Host: test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4\r\nWi");
    EXPECT_EQ(parser.parse(buffer, request), PARSE_NEED_MORE);

    // 后续输入包含多个 chunk、终止块和 trailer，验证正文拼接正确且 trailer
    // 能被完整消费，为流式上传和代理转发提供稳定解析基础。
    buffer.append("ki\r\n5\r\npedia\r\n0\r\nX-Trace: yes\r\n\r\n");
    ASSERT_EQ(parser.parse(buffer, request), PARSE_OK);
    EXPECT_EQ(request.bodyData, "Wikipedia");
    EXPECT_EQ(request.bodySize, 9U);
}

/**
 * @test HttpParserTest.ParsesByteRangesAndRejectsConflictingLengths
 * @brief 验证 HTTP Range 首部解析和冲突检测。
 *
 * 测试场景：
 *   1. 闭区间 Range (bytes=10-19)：验证 begin=10, end=19, enable=true, suffix=false
 *   2. 后缀 Range (bytes=-10)：验证 suffix=true, end=10（最后10字节）
 *   3. 多段 Range (bytes=0-1,3-4)：必须拒绝（不支持 multipart/byteranges）
 *   4. Content-Length + chunked 同时出现：必须拒绝（请求走私风险）
 *
 * 为什么重要：
 *   - Range 是断点续传（206 Partial Content）的基础
 *   - 多段 Range 会导致返回 multipart 响应，框架不支持时必须拒绝
 *   - Content-Length 与 Transfer-Encoding 冲突是 HTTP 请求走私攻击向量
 */
TEST(HttpParserTest, ParsesByteRangesAndRejectsConflictingLengths)
{
    {
        // 闭区间 Range 应保留首尾位置，供静态文件响应生成精确的 206 内容。
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append("GET /logo HTTP/1.1\r\nHost: test\r\nRange: bytes=10-19\r\n\r\n");
        ASSERT_EQ(parser.parse(buffer, request), PARSE_OK);
        EXPECT_TRUE(request.range.enable);
        EXPECT_FALSE(request.range.suffix);
        EXPECT_EQ(request.range.begin, 10U);
        EXPECT_EQ(request.range.end, 19U);
    }
    {
        // 后缀范围 "最后 N 字节" 与普通范围语义不同，单独验证 suffix 标志，
        // 防止下载续传错误地将 N 当作起始偏移。
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append("GET /logo HTTP/1.1\r\nHost: test\r\nRange: bytes=-10\r\n\r\n");
        ASSERT_EQ(parser.parse(buffer, request), PARSE_OK);
        EXPECT_TRUE(request.range.suffix);
        EXPECT_EQ(request.range.end, 10U);
    }
    {
        // 当前框架不实现 multipart/byteranges，因此必须拒绝多范围请求，
        // 避免静默只处理第一段而返回语义错误的数据。
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append("GET /logo HTTP/1.1\r\nHost: test\r\nRange: bytes=0-1,3-4\r\n\r\n");
        EXPECT_EQ(parser.parse(buffer, request), PARSE_ERROR);
    }
    {
        // Content-Length 与 chunked 同时出现会产生报文边界歧义，也是请求走私风险；
        // 解析器应在读取正文前拒绝这种组合。
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append(
            "POST / HTTP/1.1\r\n"
            "Content-Length: 1\r\n"
            "Transfer-Encoding: chunked\r\n\r\n");
        EXPECT_EQ(parser.parse(buffer, request), PARSE_ERROR);
    }
}

/**
 * @test HttpParserTest.EnforcesRequestLineHeaderAndContentLengthLimits
 * @brief 验证解析器对请求行、首部和正文大小的上限保护。
 *
 * 测试场景：
 *   1. 超长请求行（MAX_REQUEST_LINE_BYTES + 1）：立即 PARSE_ERROR
 *   2. 首部累积超限：分批追加首部行，验证跨 parse 调用的累计上限
 *   3. Content-Length 声明超限（MAX_BODY_BYTES + 1）：立即 PARSE_ERROR
 *
 * 为什么重要：
 *   - 防止攻击者通过超长请求行/首部/正文耗尽服务器内存
 *   - 上限检查必须跨增量 parse 调用累计生效
 *   - Content-Length 声明即可触发检查，服务器不会预分配不可信的长度
 */
TEST(HttpParserTest, EnforcesRequestLineHeaderAndContentLengthLimits)
{
    {
        // 无 CRLF 的超长请求行必须尽早失败，避免攻击者通过永不结束的首行占用内存。
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append(
            std::string(HttpParser::MAX_REQUEST_LINE_BYTES + 1, 'G'));
        EXPECT_EQ(parser.parse(buffer, request), PARSE_ERROR);
    }
    {
        // 分批追加合法格式的首部行，模拟慢速持续输入；循环直到状态改变，
        // 验证总首部上限跨多次 parse 累计生效，而非只限制单个网络包。
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append("GET / HTTP/1.1\r\n");
        ASSERT_EQ(parser.parse(buffer, request), PARSE_NEED_MORE);

        ParseState state = PARSE_NEED_MORE;
        const std::string headerLine = "X-Fill: " + std::string(64, 'x') + "\r\n";
        while (state == PARSE_NEED_MORE)
        {
            buffer.append(headerLine);
            state = parser.parse(buffer, request);
        }
        EXPECT_EQ(state, PARSE_ERROR);
    }
    {
        // 仅发送声明即可触发正文上限检查，确保服务器不会按不可信长度预分配
        // 或等待一个永远无法接受的巨大请求体。
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append(
            "POST / HTTP/1.1\r\nContent-Length: " +
            std::to_string(HttpParser::MAX_BODY_BYTES + 1) +
            "\r\n\r\n");
        EXPECT_EQ(parser.parse(buffer, request), PARSE_ERROR);
    }
}

/**
 * @test HttpParserTest.RejectsIncrementalChunkedBodyOverLimit
 * @brief 验证 chunked 编码正文的增量上限检查。
 *
 * 测试步骤：
 *   1. 第一块 chunk 恰好填满 MAX_BODY_BYTES，此时请求尚未结束
 *   2. 期望仍返回 PARSE_NEED_MORE（边界值保护）
 *   3. 验证 bodySize 恰好等于 MAX_BODY_BYTES
 *   4. 再增加一个字节即越界，期望返回 PARSE_ERROR
 *
 * 为什么重要：
 *   - 限制必须针对所有 chunk 的累计解码长度，不能只检查单块大小
 *   - 否则攻击者可用大量小块绕过内存保护
 *   - 边界值（恰好填满上限）必须保持 NEED_MORE 状态，不能过早报错
 */
TEST(HttpParserTest, RejectsIncrementalChunkedBodyOverLimit)
{
    HttpParser parser;
    HttpRequest request;
    Buffer buffer;
    // 第一块恰好填满上限，此时请求尚未结束，合法状态仍应是 NEED_MORE。
    // 这一区分保护边界值请求，同时验证正文计数在增量 chunk 解码中保持准确。
    buffer.append(
        "POST /upload HTTP/1.1\r\n"
        "Host: test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "100000\r\n");
    buffer.append(std::string(HttpParser::MAX_BODY_BYTES, 'x'));
    buffer.append("\r\n");
    ASSERT_EQ(parser.parse(buffer, request), PARSE_NEED_MORE);
    ASSERT_EQ(request.bodySize, HttpParser::MAX_BODY_BYTES);

    // 再增加一个字节即越界；限制必须针对所有 chunk 的累计解码长度，
    // 不能只检查单块大小，否则可用大量小块绕过内存保护。
    buffer.append("1\r\nx\r\n0\r\n\r\n");
    EXPECT_EQ(parser.parse(buffer, request), PARSE_ERROR);
}

/**
 * @test HttpParserTest.RejectsAmbiguousRequestFramingAndMissingHost
 * @brief 验证解析器对非法请求格式的拒绝。
 *
 * 拒绝场景：
 *   1. 缺少 Host 首部（HTTP/1.1 强制要求）
 *   2. 方法行后有多余 token（如 "GET / HTTP/1.1 extra"）
 *   3. Content-Length 首部名称有空格（"Content-Length : 4"）
 *   4. 重复的 Host 首部
 *
 * 为什么重要：
 *   - 严格的协议校验是安全的基础
 *   - 模糊的请求框架可能导致请求走私或缓存污染攻击
 */
TEST(HttpParserTest, RejectsAmbiguousRequestFramingAndMissingHost)
{
    const std::vector<std::string> invalidRequests{
        "GET / HTTP/1.1\r\n\r\n",              // 缺少 Host
        "GET / HTTP/1.1 extra\r\nHost: test\r\n\r\n",  // 方法行多余 token
        "POST / HTTP/1.1\r\nHost: test\r\nContent-Length : 4\r\n\r\ntest",  // 首部名有空格
        "GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n"};  // 重复 Host

    for (const auto &raw : invalidRequests)
    {
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append(raw);
        EXPECT_EQ(parser.parse(buffer, request), PARSE_ERROR) << raw;
    }
}

/**
 * @test HttpParserTest.RequiresResetAfterDeliveryAndParsesConnectionTokens
 * @brief 验证解析器 reset 机制和 Connection token 解析。
 *
 * 测试步骤：
 *   1. 发送 Connection: keep-alive, close（close 优先）
 *   2. 期望解析成功但 keepAlive() 返回 false（close 覆盖 keep-alive）
 *   3. 不 reset 直接解析下一个请求 → 期望 PARSE_ERROR
 *   4. reset 后重新解析 → 期望 PARSE_OK
 *
 * 为什么重要：
 *   - parser.reset() 必须在每次成功解析后调用，否则状态残留导致后续失败
 *   - Connection 首部的 token 处理（keep-alive, close 等）决定连接复用行为
 *   - 不 reset 直接解析是常见的 bug 来源，此测试作为安全网
 */
TEST(HttpParserTest, RequiresResetAfterDeliveryAndParsesConnectionTokens)
{
    HttpParser parser;
    HttpRequest request;
    Buffer buffer;
    buffer.append(
        "GET / HTTP/1.1\r\n"
        "Host: test\r\n"
        "Connection: keep-alive, close\r\n\r\n");

    ASSERT_EQ(parser.parse(buffer, request), PARSE_OK);
    // "close" 优先于 "keep-alive"
    EXPECT_FALSE(parser.keepAlive());
    // 不 reset 直接解析 → 期望失败（状态残留）
    EXPECT_EQ(parser.parse(buffer, request), PARSE_ERROR);

    parser.reset();
    request.reset();
    buffer.append("GET /next HTTP/1.1\r\nHost: test\r\n\r\n");
    EXPECT_EQ(parser.parse(buffer, request), PARSE_OK);
    EXPECT_EQ(request.path, "/next");
}

// ============================================================================
// ResponseSender 测试
// ============================================================================

/**
 * @test ResponseSenderTest.SendsWithoutSubReactor
 * @brief 验证 ResponseSender 可以独立于 SubReactor 工作。
 *
 * 测试步骤：
 *   1. 创建 socketpair（本地回环对），模拟客户端/服务端连接
 *   2. 创建独立的 SegmentPool、TimerWheel、ResponseSender
 *   3. 构造简单响应 "standalone sender"
 *   4. 调用 sender.send() 发送到 socket[0]
 *   5. 从 socket[1] 读取数据，验证包含状态行和响应体
 *
 * 为什么重要：
 *   - 验证 ResponseSender 完成对象化后，可以脱离 SubReactor 独立使用
 *   - 为单元测试和未来的非 Reactor 发送场景提供验证
 *   - socketpair 模拟真实 TCP 链路，验证 writev/sendfile 等系统调用
 */
TEST(ResponseSenderTest, SendsWithoutSubReactor)
{
    int sockets[2]{-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);

    SegmentPool segments;
    TimerWheel wheel(16, 5);
    ResponseSender sender(segments, wheel);
    HttpResponse response;
    response.keepAlive = false;
    response.text("standalone sender");
    response.buildHeader();

    EXPECT_EQ(sender.send(sockets[0], response), SEND_OK);
    ASSERT_EQ(shutdown(sockets[0], SHUT_WR), 0);

    std::string wire;
    char received[512]{};
    while (const ssize_t size = read(sockets[1], received, sizeof(received)))
    {
        ASSERT_GT(size, 0);
        wire.append(received, static_cast<size_t>(size));
    }
    EXPECT_NE(wire.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(wire.find("standalone sender"), std::string::npos);

    close(sockets[0]);
    close(sockets[1]);
}

// ============================================================================
// HttpRange 测试
// ============================================================================

/**
 * @test HttpRangeTest.ResolvesClosedOpenSuffixAndInvalidRanges
 * @brief 验证 resolveByteRange() 对各种 Range 格式的解析。
 *
 * 测试场景：
 *   1. 闭区间 (10-19)：begin=10, end=19（包含式 end）
 *   2. 开区间 (25-)：begin=25, end=99（延伸到资源末尾）
 *   3. 后缀范围 (-10)：begin=90, end=99（从末尾反推）
 *   4. 后缀范围超量 (-200, 资源100)：begin=0, end=99（退化为整个资源）
 *   5. 越界范围 (100-, 资源100)：返回 false（不可满足）
 *   6. 空资源 (0 字节)：返回 false
 *
 * 为什么重要：
 *   - Range 解析是断点续传（206）的基础
 *   - 边界条件（空资源、超量后缀）容易出现 size_t 下溢
 *   - HTTP 规范要求超量后缀退化为整个资源
 */
TEST(HttpRangeTest, ResolvesClosedOpenSuffixAndInvalidRanges)
{
    size_t begin = 0;
    size_t end = 0;

    // 闭区间直接映射，验证 HTTP 的包含式 end 语义没有被误作半开区间。
    RangeInfo closed{true, 10, false, 19};
    EXPECT_TRUE(resolveByteRange(closed, 100, begin, end));
    EXPECT_EQ(begin, 10U);
    EXPECT_EQ(end, 19U);

    // 省略结尾时应延伸到资源末尾，为断点续传提供符合规范的长度计算。
    RangeInfo open{true, 25, false, SIZE_MAX};
    EXPECT_TRUE(resolveByteRange(open, 100, begin, end));
    EXPECT_EQ(begin, 25U);
    EXPECT_EQ(end, 99U);

    // 后缀范围从总长度反推起点；请求长度超过资源时应退化为整个资源，
    // 避免 size_t 下溢并保持客户端可预测行为。
    RangeInfo suffix{true, 0, true, 10};
    EXPECT_TRUE(resolveByteRange(suffix, 100, begin, end));
    EXPECT_EQ(begin, 90U);
    EXPECT_EQ(end, 99U);

    suffix.end = 200;
    EXPECT_TRUE(resolveByteRange(suffix, 100, begin, end));
    EXPECT_EQ(begin, 0U);
    EXPECT_EQ(end, 99U);

    // 起点等于资源长度及空资源均不可满足，覆盖最容易出现越界的边界条件。
    RangeInfo outOfRange{true, 100, false, SIZE_MAX};
    EXPECT_FALSE(resolveByteRange(outOfRange, 100, begin, end));
    EXPECT_FALSE(resolveByteRange(closed, 0, begin, end));
}

/**
 * @test HttpRangeTest.PreservesContentRangeForMemoryAndFileBodies
 * @brief 验证 Content-Range 头在 buildHeader 后被保留。
 *
 * 测试场景：
 *   1. 内存正文路径：206 响应 + Content-Range 头
 *      - 验证 buildHeader() 后 Content-Range 仍然存在
 *      - 防止自动构造覆盖业务层显式设置的头
 *   2. 文件正文路径：206 响应 + Content-Range 头
 *      - 验证只出现一次（避免重复头导致客户端困惑）
 *
 * 为什么重要：
 *   - Content-Range 是 206 Partial Content 响应的核心头
 *   - 框架自动构造不应覆盖业务层显式设置的头
 *   - 文件正文走独立路径，需单独验证
 */
TEST(HttpRangeTest, PreservesContentRangeForMemoryAndFileBodies)
{
    // 内存正文路径必须保留业务层显式设置的 Content-Range，防止统一响应头构造
    // 在自动补 Content-Length 时覆盖范围元数据。
    HttpResponse memoryResponse;
    memoryResponse.status = 206;
    memoryResponse.statusText = "Partial Content";
    memoryResponse.text("0123456789");
    memoryResponse.setHeader("Content-Range", "bytes 10-19/100");
    EXPECT_NE(
        memoryResponse.buildHeader().find("Content-Range: bytes 10-19/100\r\n"),
        std::string::npos);

    // 文件正文走独立的发送路径；除验证保留外，还检查只出现一次，避免框架
    // 自动头与用户头重复，造成客户端选择不同值。
    HttpResponse fileResponse;
    fileResponse.status = 206;
    fileResponse.statusText = "Partial Content";
    auto fileBody = std::make_shared<FileBody>();
    fileBody->remain_ = 10;
    fileResponse.body = fileBody;
    fileResponse.setHeader("Content-Range", "bytes 10-19/100");
    const std::string fileHeader = fileResponse.buildHeader();
    EXPECT_NE(
        fileHeader.find("Content-Range: bytes 10-19/100\r\n"),
        std::string::npos);
    // 确保只出现一次：第一次 find 位置之后不存在第二次
    EXPECT_EQ(
        fileHeader.find(
            "Content-Range: bytes 10-19/100\r\n",
            fileHeader.find("Content-Range: bytes 10-19/100\r\n") + 1),
        std::string::npos);
}

// ============================================================================
// Router 测试
// ============================================================================

/**
 * @test RouterTest.MatchesDynamicRouteAndExtractsParameter
 * @brief 验证动态路由匹配和参数提取。
 *
 * 测试步骤：
 *   1. 注册路由 GET /user/:id，handler 返回 id 参数值
 *   2. 构造请求 path="/user/42"
 *   3. 验证 router.handle() 返回 true
 *   4. 验证 context.handled 为 true
 *   5. 验证 context.param("id") 返回 "42"
 *   6. 验证响应体为 "42"
 *
 * 为什么重要：
 *   - 动态路由（如 /user/:id）是 REST API 的核心
 *   - 参数提取必须写入 context，供 handler 使用
 *   - 响应对象必须由 Router 正确构造
 */
TEST(RouterTest, MatchesDynamicRouteAndExtractsParameter)
{
    // 验证动态段既参与匹配又写入上下文，并可在处理器中直接生成响应；
    // 这是后续 REST 路由复用参数提取机制的契约测试。
    Router router;
    router.GET("/user/:id", [](RequestContext& ctx) {
        ctx.response->text(ctx.param("id"));
        return true;
    });

    HttpResponse response;
    RequestContext context;
    context.response = &response;
    context.request.method = "GET";
    context.request.path = "/user/42";

    EXPECT_TRUE(router.handle(context));
    EXPECT_TRUE(context.handled);
    EXPECT_EQ(context.param("id"), "42");
    EXPECT_EQ(responseBody(response), "42");
}

/**
 * @test RouterTest.MiddlewareCanShortCircuitRoute
 * @brief 验证中间件可以短路路由链路。
 *
 * 测试步骤：
 *   1. 注册中间件：不调用 next()，直接返回 401
 *   2. 注册路由 GET /admin，设置 routeCalled 标志
 *   3. 发送请求到 /admin
 *   4. 验证 router.handle() 返回 true（中间件已处理）
 *   5. 验证 routeCalled 仍为 false（handler 未执行）
 *   6. 验证响应状态码为 401
 *
 * 为什么重要：
 *   - 中间件短路是认证、限流等横切逻辑的基础
 *   - 中间件不调用 next() 时，handler 不应执行
 *   - routeCalled 变量可检测意外穿透
 */
TEST(RouterTest, MiddlewareCanShortCircuitRoute)
{
    // 中间件不调用 next 即应终止链路，路由处理器不得执行。该能力是认证、
    // 限流等横切逻辑阻断请求的基础，routeCalled 可检测意外穿透。
    Router router;
    bool routeCalled = false;
    router.use([](RequestContext& ctx, std::function<void()>) {
        ctx.response->status = 401;
        ctx.response->statusText = "Unauthorized";
        ctx.response->text("Unauthorized");
    });
    router.GET("/admin", [&](RequestContext&) {
        routeCalled = true;
        return true;
    });

    HttpResponse response;
    RequestContext context;
    context.response = &response;
    context.request.method = "GET";
    context.request.path = "/admin";

    EXPECT_TRUE(router.handle(context));
    EXPECT_FALSE(routeCalled);
    EXPECT_EQ(response.status, 401);
    EXPECT_EQ(responseBody(response), "Unauthorized");
}

/**
 * @test RouterTest.ProducesNotFoundResponse
 * @brief 验证路由未匹配时返回 404。
 *
 * 测试步骤：
 *   1. 创建空 Router（未注册任何路由）
 *   2. 发送请求到 /missing
 *   3. 验证 router.handle() 返回 true（Router 已处理）
 *   4. 验证 context.handled 为 true
 *   5. 验证响应状态码为 404，状态文本为 "Not Found"
 *
 * 为什么重要：
 *   - 未匹配路径必须由 Router 形成完整 404 响应
 *   - 确保上层连接代码不会重复派发或遗漏响应
 *   - 标记 handled=true 防止上层再次处理
 */
TEST(RouterTest, ProducesNotFoundResponse)
{
    // 未匹配路径仍由 Router 形成完整 404，并标记已处理，确保上层连接代码
    // 不会重复派发或遗漏响应。
    Router router;
    HttpResponse response;
    RequestContext context;
    context.response = &response;
    context.request.method = "GET";
    context.request.path = "/missing";

    EXPECT_TRUE(router.handle(context));
    EXPECT_TRUE(context.handled);
    EXPECT_EQ(response.status, 404);
    EXPECT_EQ(response.statusText, "Not Found");
    EXPECT_EQ(responseBody(response), "Not Found");
}

}  // namespace
