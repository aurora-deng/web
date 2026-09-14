// ============================================================
// 文件名：unit_tests.cpp
// ------------------------------------------------------------
// 【职责比喻：质检车间】
//   服务器是一座工厂，本文件就是工厂的"质检车间"——把每个零件单独拿到
//   显微镜下检查。如果直接组装完再测试，出了问题很难定位是哪个零件坏了。
//   所以质检员会用确定的小输入逐一验证每个基础组件：
//   "这个齿轮（Buffer）转 10 圈会不会卡？""发条（HttpParser）上紧后能跑多久？"
//   "表盘指针（Router）指对了吗？"这就是单元测试——用最小的、确定性的输入，
//   验证每个基础组件的行为符合预期。
//
//   这些测试刻意使用小而确定的输入定位协议边界，既为框架重构提供快速反馈，
//   也避免只依赖端到端测试时难以区分解析、响应构造和路由层故障。
//
// 关键技术点（初学者重点理解）：
//   1. EXPECT vs ASSERT：EXPECT_EQ 失败后继续执行当前用例（适合非关键断言），
//      ASSERT_EQ 失败立即终止当前用例（适合后续依赖此断言的场景，如指针非空）。
//   2. 半包/粘包测试：模拟 TCP 分片到达，验证解析器能跨多次 parse 调用保持状态。
//   3. 边界值测试：恰好填满上限（合法）vs 超出上限一个字节（非法），验证边界判断。
//   4. 安全测试：请求走私（Content-Length + chunked 冲突）、超长请求行内存耗尽攻击。
//   5. RFC 黄金对照：WebSocket 握手用 RFC 6455 官方示例做"黄金对照"，
//      确保协议实现与标准完全一致。
//
// 测试框架：Google Test (gtest)
// 运行方式：ctest --test-dir build-tests --output-on-failure
//
// 测试覆盖：
//   - Buffer          : 追加、压缩、分隔符查找
//   - HttpParser      : 半包/粘包/chunked/range/长度限制/请求走私/reset
//   - TransportWriter : 独立于 SubReactor 的发送验证（见 transport_tests.cpp）
//   - HttpRange       : 通过 HttpParser 验证 Range 首部；Content-Range 保留
//   - Router          : 动态路由/中间件短路/404
//   - WebSocket       : 握手密钥计算/编解码往返/掩码校验/半包解析（第四阶段新增）
// ============================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "server/Buffer/Buffer.h"
#include "server/response/FileBody.h"
#include "server/response/StringBody.h"
#include "server/Route/Router.h"
#include "server/http/http.h"
#include "server/http/HttpParser/HttpParser.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/SegmentPool/SegmentPool.h"
#include "server/timer/TimeWheel.h"

#include <utility>
// ----------------------------------------------------------------------------
// gtest 断言宏速查（初学者重点理解）
//   单元测试 = 预先定义"程序应该产生什么行为"，然后自动验证实际行为是否符合预期。
//   - EXPECT_* 失败后继续执行当前用例（适合非关键断言，收集更多失败信息）
//   - ASSERT_* 失败立即终止当前用例（适合后续逻辑依赖此断言的场景，如指针非空）
//
//   EXPECT_EQ(a,b) / ASSERT_EQ(a,b)   ——  等于
//   EXPECT_NE(a,b) / ASSERT_NE(a,b)   ——  不等于
//   EXPECT_LT(a,b) / ASSERT_LT(a,b)   ——  小于
//   EXPECT_LE(a,b) / ASSERT_LE(a,b)   ——  小于等于
//   EXPECT_GT(a,b) / ASSERT_GT(a,b)   ——  大于
//   EXPECT_GE(a,b) / ASSERT_GE(a,b)   ——  大于等于
// ----------------------------------------------------------------------------


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

// 非 const 对象上存在 void buildHeader() 重载时会优先于返回 string 的 const 版本；
// 单测需要检查首部文本，必须走 const 重载。
std::string headerText(const HttpResponse& response)
{
    return response.buildHeader();
}

} // namespace

// ============================================================================
// Buffer 测试
// ============================================================================

/**
 * @test BufferTest.AppendsCompactsAndFindsDelimiters
 * @brief 验证  er 的追加、压缩和分隔符查找功能。
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
    // 测试连续append，和超容量测试
    buffer.append("abcd", 4);
    // 剩余检查
    buffer.retrieve(3);
    buffer.append("efghij", 6);

    EXPECT_EQ(buffer.readableBytes(), 7U);
    EXPECT_EQ(std::string(buffer.peek(), buffer.readableBytes()), "defghij");

    buffer.clear();
    // 同时检查行结束符和首部结束符的相对位置，防止查找函数越过可读边界，
    // 或在多段协议分隔符中返回错误位置。
    buffer.append("one\r\ntwo\r\n\r\n", 12);
    // 失败立即停止：
    ASSERT_NE(buffer.findCRLF(), buffer.beginWrite());
    ASSERT_NE(buffer.findCRLFCRLF(), nullptr);
    // 失败后继续
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
// 测试根据程序跑到流程去你要测试的去测试
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
// HttpRange 测试
// ============================================================================

/**
 * @test HttpRangeTest.ParsesClosedOpenAndSuffixRangesViaParser
 * @brief 通过 HttpParser 验证 Range 首部解析（本仓库无独立 resolveByteRange API）。
 *
 * 当前实现把闭区间/后缀 Range 解析落在 HttpParser 内，字节换算在 sendfile 路径完成。
 * 单测只验证解析器产出的 RangeInfo，不依赖不存在的自由函数。
 */
TEST(HttpRangeTest, ParsesClosedOpenAndSuffixRangesViaParser)
{
    {
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
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append("GET /logo HTTP/1.1\r\nHost: test\r\nRange: bytes=25-\r\n\r\n");
        ASSERT_EQ(parser.parse(buffer, request), PARSE_OK);
        EXPECT_TRUE(request.range.enable);
        EXPECT_FALSE(request.range.suffix);
        EXPECT_EQ(request.range.begin, 25U);
    }
    {
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append("GET /logo HTTP/1.1\r\nHost: test\r\nRange: bytes=-10\r\n\r\n");
        ASSERT_EQ(parser.parse(buffer, request), PARSE_OK);
        EXPECT_TRUE(request.range.enable);
        EXPECT_TRUE(request.range.suffix);
        EXPECT_EQ(request.range.end, 10U);
    }
    {
        HttpParser parser;
        HttpRequest request;
        Buffer buffer;
        buffer.append("GET /logo HTTP/1.1\r\nHost: test\r\nRange: bytes=0-1,3-4\r\n\r\n");
        EXPECT_EQ(parser.parse(buffer, request), PARSE_ERROR);
    }
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
        headerText(memoryResponse).find("Content-Range: bytes 10-19/100\r\n"),
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
    const std::string fileHeader = headerText(fileResponse);
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
 *   5. 验证 context.response 上状态码为 404、body 为 "Not Found"
 *
 * 为什么重要：
 *   - 未匹配路径必须由 Router 形成完整 404 响应
 *   - 确保上层连接代码不会重复派发或遗漏响应
 *   - 标记 handled=true 防止上层再次处理
 */
TEST(RouterTest, ProducesNotFoundResponse)
{
    // make404() 会从 responsePool 取出新对象并改写 ctx.response，
    // 因此断言必须看 context.response，而不是测试栈上原先那份 HttpResponse。
    // 另外当前 make404 只设置 status=404 与 body，不改 statusText。
    Router router;
    HttpResponse stackResponse;
    RequestContext context;
    context.response = &stackResponse;
    context.request.method = "GET";
    context.request.path = "/missing";

    EXPECT_TRUE(router.handle(context));
    EXPECT_TRUE(context.handled);
    ASSERT_NE(context.response, nullptr);
    EXPECT_EQ(context.response->status, 404);
    EXPECT_EQ(responseBody(*context.response), "Not Found");
}

// ---------------------------------------------------------------------------
// WebSocket 测试（第四阶段新增）
// ---------------------------------------------------------------------------
// WebSocket 是独立于 HTTP 的二进制帧协议（RFC 6455）。以下测试覆盖握手、
// 编解码、增量解析、消息分发和升级标志五个层面，与 HttpParser 测试对称。
// 注意：WebSocket 的 include 集中放在此处而非文件头，是为了让上面的 HTTP
// 测试区在阅读时不被 WebSocket 头文件干扰，体现"按协议分块"的组织方式。
#include "server/websocket/WebSocketHandshake/WebSocketHandshake.h"
#include "server/websocket/WebSocketCodec/WebSocketCodec.h"
#include "server/websocket/WebSocketDelivery/WebSocketDeliveryTracker.h"
#include "server/websocket/WebSocketParser/WebSocketParser.h"
#include "server/websocket/WebSocketMessageAssembler/WebSocketMessageAssembler.h"
#include "server/websocket/WebSocketValidation/WebSocketValidation.h"
#include "server/websocket/WebSocketDispatcher/WebSocketDispatcher.h"
#include "server/websocket/WebSocketSession/WebSocketSession.h"
#include "server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"

/**
 * @test WebSocketHandshakeTest.AcceptKeyMatchesRfc6455Example
 * @brief 用 RFC 6455 第 4.2.2 节的官方示例验证 Sec-WebSocket-Accept 计算正确。
 *
 * 测试步骤：
 *   1. 取 RFC 6455 §1.3 / §4.2.2 给出的官方示例客户端密钥 "dGhlIHNhbXBsZSBub25jZQ=="
 *   2. 调用 WebSocketHandshake::computeAcceptKey() 计算 Accept 密钥
 *   3. 验证结果等于 RFC 文档给出的固定值 "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
 *
 * 为什么重要：
 *   - RFC 6455 §1.3 规定 Accept 计算公式：Base64(SHA1(key + GUID))
 *   - GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" 是固定魔法值，所有实现必须一致
 *   - 用官方示例做"黄金对照"，确保握手实现与协议标准完全一致
 *   - 任何字节级偏差都会导致浏览器拒绝握手（101 响应被判定非法）
 */
TEST(WebSocketHandshakeTest, AcceptKeyMatchesRfc6455Example)
{
    // RFC 6455 §1.3 / §4.2.2 示例
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    EXPECT_EQ(WebSocketHandshake::computeAcceptKey(key),
              "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

/**
 * @test WebSocketHandshakeTest.ValidateRequiresVersion13
 * @brief 验证握手必须要求 Sec-WebSocket-Version: 13。
 *
 * 测试步骤：
 *   1. 构造完整握手请求，但 version=12（非 13），期望 validate() 返回 ok=false
 *   2. 改 version=13，期望 validate() 返回 ok=true 且 acceptKey 正确
 *
 * 为什么重要：
 *   - RFC 6455 §4.1 规定：Sec-WebSocket-Version 必须为 13，其他版本必须拒绝
 *   - 接受错误版本会导致协议不兼容（如旧版 Hixie-76 有完全不同的帧格式）
 *   - 同时验证合法路径下 acceptKey 与上一个测试的黄金对照值一致
 */
TEST(WebSocketHandshakeTest, ValidateRequiresVersion13)
{
    HttpRequest req;
    req.method = "GET";
    req.version = "HTTP/1.1";
    req.headers["upgrade"] = "websocket";
    req.headers["connection"] = "Upgrade";
    req.headers["sec-websocket-key"] = "dGhlIHNhbXBsZSBub25jZQ==";
    req.headers["sec-websocket-version"] = "12";
    auto bad = WebSocketHandshake::validate(req);
    EXPECT_FALSE(bad.ok);

    req.headers["sec-websocket-version"] = "13";
    auto ok = WebSocketHandshake::validate(req);
    EXPECT_TRUE(ok.ok);
    EXPECT_EQ(ok.acceptKey, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

/**
 * @test WebSocketCodecTest.EncodeDecodeTextRoundTrip
 * @brief 文本帧编解码往返测试：服务端编码 → 客户端掩码帧 → 解码还原。
 *
 * 测试步骤：
 *   1. 调用 encodeText() 编码 "hello-ws"，验证 opcode=Text 且服务端帧无 mask
 *   2. 手工构造带掩码的客户端帧（FIN=1, opcode=Text, MASK=1）写入 Buffer
 *   3. 调用 codec.decode() 解码，验证还原出原始 payload "hello-ws"
 *   4. 验证 Buffer 被完全消费（readableBytes()==0）
 *
 * 为什么重要：
 *   - RFC 6455 §5.3 规定帧格式：FIN+opcode+MASK+payload_len+mask_key+payload
 *   - RFC 6455 §5.1 规定：客户端→服务端的帧必须掩码，服务端→客户端必须不掩码
 *   - 往返测试同时覆盖编码（服务端侧）和解码（客户端侧）两个方向
 *   - 掩码算法 payload[i] ^= mask[i % 4] 必须验证解掩码后内容正确
 */
TEST(WebSocketCodecTest, EncodeDecodeTextRoundTrip)
{
    // 服务端编码（无 mask）；解码侧模拟客户端掩码帧。
    const std::string payload = "hello-ws";
    auto encoded = WebSocketCodec::encodeText(payload);
    ASSERT_GE(encoded.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(encoded[0]) & 0x0F, 0x01); // text
    EXPECT_EQ(static_cast<uint8_t>(encoded[1]) & 0x80, 0);     // server unmasked

    // 构造带 mask 的客户端帧塞进 Buffer
    Buffer buf;
    const uint8_t mask[4] = {0x01, 0x02, 0x03, 0x04};
    std::string masked = payload;
    for (size_t i = 0; i < masked.size(); ++i)
        masked[i] = static_cast<char>(static_cast<uint8_t>(masked[i]) ^ mask[i % 4]);

    char header[2] = {static_cast<char>(0x81), static_cast<char>(0x80 | payload.size())};
    buf.append(header, 2);
    buf.append(reinterpret_cast<const char *>(mask), 4);
    buf.append(masked.data(), masked.size());

    WebSocketDispatcher dispatcher;
    WebSocketCodec codec(dispatcher);
    WebSocketParser parser;
    WsFrame frame;
    EXPECT_EQ(codec.decode(buf, parser, frame), WsDecodeResult::Ok);
    EXPECT_TRUE(frame.fin);
    EXPECT_EQ(frame.opcode, WsOpcode::Text);
    EXPECT_EQ(frame.payload, payload);
    EXPECT_EQ(buf.readableBytes(), 0u);
}

/**
 * @test WebSocketCodecTest.RejectUnmaskedClientFrame
 * @brief 验证服务端拒绝未加掩码的客户端帧（RFC 6455 5.1 强制要求客户端必须掩码）。
 *
 * 测试步骤：
 *   1. 构造一个 FIN=1, opcode=Text, MASK=0 的"客户端帧"（未掩码）
 *   2. 调用 codec.decode()，期望返回 WsDecodeResult::Error
 *
 * 为什么重要：
 *   - RFC 6455 §5.1 强制要求：客户端发送的每一帧都必须加掩码（MASK 位=1）
 *   - 接受未掩码的客户端帧是协议违规，可能导致中间代理缓存污染攻击
 *   - 这是 WebSocket 安全性的基础检查，防止恶意客户端绕过掩码保护
 */
TEST(WebSocketCodecTest, RejectUnmaskedClientFrame)
{
    Buffer buf;
    char raw[] = {static_cast<char>(0x81), 0x05, 'h', 'e', 'l', 'l', 'o'};
    buf.append(raw, sizeof(raw));
    WebSocketDispatcher dispatcher;
    WebSocketCodec codec(dispatcher);
    WebSocketParser parser;
    WsFrame frame;
    EXPECT_EQ(codec.decode(buf, parser, frame), WsDecodeResult::Error);
}

/**
 * @test WebSocketParserTest.NeedMoreThenComplete
 * @brief 增量解析测试：半包 → 补齐 → 完成。
 *
 * 测试步骤：
 *   1. 构造一个完整的带掩码文本帧（payload="ab"）
 *   2. 第一次只写入 1 字节（基础头的第一字节），期望返回 NeedMore
 *   3. 追加剩余字节（第二字节 + 4 字节 mask + 掩码后 payload）
 *   4. 期望返回 Ok，且 frame.payload == "ab"
 *
 * 为什么重要：
 *   - TCP 是字节流，帧可能跨多次 read 到达（半包）
 *   - 解析器必须能"暂停"在半包状态，等数据补齐后继续推进状态机
 *   - 这与 HttpParser 的半包处理对称，是增量解析的核心能力
 *   - WebSocketParser 的状态机（BASE_HEADER→EXT_LENGTH→MASK_KEY→PAYLOAD→COMPLETE）
 *     使得半包恢复成为可能
 */
TEST(WebSocketParserTest, NeedMoreThenComplete)
{
    const std::string payload = "ab";
    const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    std::string masked = payload;
    for (size_t i = 0; i < masked.size(); ++i)
        masked[i] = static_cast<char>(static_cast<uint8_t>(masked[i]) ^ mask[i % 4]);

    Buffer buf;
    char header[2] = {static_cast<char>(0x81), static_cast<char>(0x80 | payload.size())};
    buf.append(header, 1); // 半包：仅首字节

    WebSocketDispatcher dispatcher;
    WebSocketCodec codec(dispatcher);
    WebSocketParser parser;
    WsFrame frame;
    EXPECT_EQ(codec.decode(buf, parser, frame), WsDecodeResult::NeedMore);

    buf.append(header + 1, 1);
    buf.append(reinterpret_cast<const char *>(mask), 4);
    buf.append(masked.data(), masked.size());
    EXPECT_EQ(codec.decode(buf, parser, frame), WsDecodeResult::Ok);
    EXPECT_EQ(frame.payload, payload);
}

/**
 * @test WebSocketParserTest.RejectsReservedOpcodeAndFragmentedControl
 * @brief 帧头一到齐就拒绝保留 opcode，以及 FIN=0 的控制帧。
 */
TEST(WebSocketParserTest, RejectsReservedOpcodeAndFragmentedControl)
{
    {
        Buffer buf;
        const char reserved[] = {
            static_cast<char>(0x83), // FIN=1, opcode=0x3（保留）
            static_cast<char>(0x80)  // MASK=1, payload=0
        };
        buf.append(reserved, sizeof(reserved));
        WebSocketParser parser;
        WsFrame frame;
        EXPECT_EQ(parser.parse(buf, frame), WsDecodeResult::Error);
    }
    {
        Buffer buf;
        const char fragmentedPing[] = {
            static_cast<char>(0x09), // FIN=0, opcode=Ping
            static_cast<char>(0x80)  // MASK=1, payload=0
        };
        buf.append(fragmentedPing, sizeof(fragmentedPing));
        WebSocketParser parser;
        WsFrame frame;
        EXPECT_EQ(parser.parse(buf, frame), WsDecodeResult::Error);
    }
}

/**
 * @test WebSocketParserTest.RejectsNonCanonicalExtendedLengths
 * @brief 长度必须使用最短编码：125 不能伪装成 126，65535 不能伪装成 127。
 */
TEST(WebSocketParserTest, RejectsNonCanonicalExtendedLengths)
{
    {
        Buffer buf;
        const char nonCanonical126[] = {
            static_cast<char>(0x81), static_cast<char>(0xFE),
            0x00, 0x7D // 扩展值 125，本应直接写在 7 位长度中
        };
        buf.append(nonCanonical126, sizeof(nonCanonical126));
        WebSocketParser parser;
        WsFrame frame;
        EXPECT_EQ(parser.parse(buf, frame), WsDecodeResult::Error);
    }
    {
        Buffer buf;
        const char nonCanonical127[] = {
            static_cast<char>(0x81), static_cast<char>(0xFF),
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            static_cast<char>(0xFF), static_cast<char>(0xFF)
        };
        buf.append(nonCanonical127, sizeof(nonCanonical127));
        WebSocketParser parser;
        WsFrame frame;
        EXPECT_EQ(parser.parse(buf, frame), WsDecodeResult::Error);
    }
}

/**
 * @test WebSocketMessageAssemblerTest.EmptyFirstFragmentIsStillActive
 * @brief 空 payload 的首片仍然开启了一条分片消息，不能用 string::empty 判断状态。
 */
TEST(WebSocketMessageAssemblerTest, EmptyFirstFragmentIsStillActive)
{
    WebSocketMessageAssembler assembler;
    WsFrame complete;

    WsFrame first;
    first.fin = false;
    first.opcode = WsOpcode::Text;
    EXPECT_EQ(assembler.consume(std::move(first), complete),
              WsAssemblyResult::Incomplete);
    EXPECT_TRUE(assembler.hasPendingMessage());
    EXPECT_EQ(assembler.pendingBytes(), 0U);

    WsFrame last;
    last.fin = true;
    last.opcode = WsOpcode::Continuation;
    last.payload = "done";
    EXPECT_EQ(assembler.consume(std::move(last), complete),
              WsAssemblyResult::Complete);
    EXPECT_EQ(complete.opcode, WsOpcode::Text);
    EXPECT_EQ(complete.payload, "done");
    EXPECT_FALSE(assembler.hasPendingMessage());
}

/**
 * @test WebSocketMessageAssemblerTest.RejectsInterleavingAndCapsWholeMessage
 * @brief 分片未结束不能插入新数据首帧，且总消息大小不能靠拆帧绕过。
 */
TEST(WebSocketMessageAssemblerTest, RejectsInterleavingAndCapsWholeMessage)
{
    WsFrame complete;
    {
        WebSocketMessageAssembler assembler;
        WsFrame first{false, WsOpcode::Text, true, "part"};
        EXPECT_EQ(assembler.consume(std::move(first), complete),
                  WsAssemblyResult::Incomplete);
        WsFrame interleaved{true, WsOpcode::Binary, true, "new"};
        EXPECT_EQ(assembler.consume(std::move(interleaved), complete),
                  WsAssemblyResult::ProtocolError);
    }
    {
        WebSocketMessageAssembler assembler(5);
        WsFrame first{false, WsOpcode::Text, true, "abc"};
        EXPECT_EQ(assembler.consume(std::move(first), complete),
                  WsAssemblyResult::Incomplete);
        WsFrame overflow{true, WsOpcode::Continuation, true, "def"};
        EXPECT_EQ(assembler.consume(std::move(overflow), complete),
                  WsAssemblyResult::MessageTooBig);
        EXPECT_FALSE(assembler.hasPendingMessage());
    }
}

/**
 * @test WebSocketValidationTest.ValidatesCompleteUtf8IncludingSplitCodePoint
 * @brief UTF-8 应在完整消息上校验；一个中文字的三个字节允许跨两个 WS 分片。
 */
TEST(WebSocketValidationTest, ValidatesCompleteUtf8IncludingSplitCodePoint)
{
    EXPECT_TRUE(isValidWebSocketUtf8("plain ASCII"));
    EXPECT_TRUE(isValidWebSocketUtf8(std::string("\xE4\xB8\xAD", 3)));

    EXPECT_FALSE(isValidWebSocketUtf8(std::string("\xC0\xAF", 2)));       // 过长编码
    EXPECT_FALSE(isValidWebSocketUtf8(std::string("\xED\xA0\x80", 3))); // surrogate
    EXPECT_FALSE(isValidWebSocketUtf8(std::string("\xF4\x90\x80\x80", 4))); // > U+10FFFF
    EXPECT_FALSE(isValidWebSocketUtf8(std::string("\xE2\x82", 2)));      // 截断

    WebSocketMessageAssembler assembler;
    WsFrame complete;
    WsFrame first{false, WsOpcode::Text, true, std::string("\xE4", 1)};
    WsFrame last{true, WsOpcode::Continuation, true, std::string("\xB8\xAD", 2)};
    EXPECT_EQ(assembler.consume(std::move(first), complete),
              WsAssemblyResult::Incomplete);
    EXPECT_EQ(assembler.consume(std::move(last), complete),
              WsAssemblyResult::Complete);
    EXPECT_TRUE(isValidWebSocketUtf8(complete.payload));
}

/**
 * @test WebSocketValidationTest.ParsesClosePayloadByFailureKind
 * @brief Close 的结构/状态码错误属于 1002，reason 的 UTF-8 错误属于 1007。
 */
TEST(WebSocketValidationTest, ParsesClosePayloadByFailureKind)
{
    WsCloseInfo info;
    EXPECT_EQ(parseWebSocketClosePayload({}, info), WsClosePayloadResult::Ok);
    EXPECT_FALSE(info.hasCode);

    EXPECT_EQ(parseWebSocketClosePayload(std::string("\x03", 1), info),
              WsClosePayloadResult::ProtocolError);
    EXPECT_EQ(parseWebSocketClosePayload(std::string("\x03\xED", 2), info), // 1005
              WsClosePayloadResult::ProtocolError);

    EXPECT_EQ(parseWebSocketClosePayload(std::string("\x03\xF6ok", 4), info), // 1014
              WsClosePayloadResult::Ok);
    EXPECT_TRUE(info.hasCode);
    EXPECT_EQ(info.code, 1014);
    EXPECT_EQ(info.reason, "ok");

    EXPECT_EQ(parseWebSocketClosePayload(std::string("\x0F\xA0", 2), info), // 4000
              WsClosePayloadResult::Ok);
    EXPECT_EQ(info.code, 4000);

    std::string invalidReason("\x03\xE8", 2); // 1000
    invalidReason.append("\xC0\xAF", 2);
    EXPECT_EQ(parseWebSocketClosePayload(invalidReason, info),
              WsClosePayloadResult::InvalidUtf8);
}

/**
 * @test WebSocketValidationTest.AcceptsOnlyWireCloseCodeRanges
 * @brief 把标准码、内部保留码、注册区间与私有区间的边界固定为回归测试。
 */
TEST(WebSocketValidationTest, AcceptsOnlyWireCloseCodeRanges)
{
    EXPECT_FALSE(isValidWebSocketCloseCode(999));
    EXPECT_TRUE(isValidWebSocketCloseCode(1000));
    EXPECT_FALSE(isValidWebSocketCloseCode(1004));
    EXPECT_FALSE(isValidWebSocketCloseCode(1005));
    EXPECT_FALSE(isValidWebSocketCloseCode(1006));
    EXPECT_TRUE(isValidWebSocketCloseCode(1014));
    EXPECT_FALSE(isValidWebSocketCloseCode(1015));
    EXPECT_FALSE(isValidWebSocketCloseCode(2999));
    EXPECT_TRUE(isValidWebSocketCloseCode(3000));
    EXPECT_TRUE(isValidWebSocketCloseCode(4999));
    EXPECT_FALSE(isValidWebSocketCloseCode(5000));
}

/**
 * @test WebSocketCodecTest.RejectsInvalidOutboundTextAndControlFrames
 * @brief 编码器是出站闸门，不能由本服务器制造非法控制帧或非法完整文本消息。
 */
TEST(WebSocketCodecTest, RejectsInvalidOutboundTextAndControlFrames)
{
    EXPECT_TRUE(WebSocketCodec::encodeText(std::string("\xC0\xAF", 2)).empty());
    EXPECT_TRUE(WebSocketCodec::encodePing(std::string(126, 'p')).empty());
    EXPECT_TRUE(WebSocketCodec::encodeClose(1005).empty());
    EXPECT_TRUE(WebSocketCodec::encodeClose(1000, std::string(124, 'r')).empty());

    WsFrame fragmentedPing{false, WsOpcode::Ping, false, "ping"};
    EXPECT_TRUE(WebSocketCodec::encode(fragmentedPing).empty());
    WsFrame invalidClose{true, WsOpcode::Close, false, std::string("\x03\xED", 2)};
    EXPECT_TRUE(WebSocketCodec::encode(invalidClose).empty());

    const auto emptyClose = WebSocketCodec::encodeClose();
    ASSERT_EQ(emptyClose.size(), 2U);
    EXPECT_EQ(static_cast<uint8_t>(emptyClose[0]), 0x88);
    EXPECT_EQ(static_cast<uint8_t>(emptyClose[1]), 0x00);
}

/**
 * @test WebSocketCodecTest.ApplicationEnvelopeRoundTripsEscapedFields
 * @brief 验证业务信封与 RFC 帧相互独立，JSON 转义和关联字段不会在往返中丢失。
 */
TEST(WebSocketCodecTest, ApplicationEnvelopeRoundTripsEscapedFields)
{
    WebSocketMessage outbound;
    outbound.version = 2;
    outbound.type = "chat";
    outbound.messageId = "server-\"42\"";
    outbound.replyTo = "client\\7";
    outbound.status = "accepted";
    outbound.text = "line one\nline two: \xE4\xBD\xA0\xE5\xA5\xBD";
    outbound.fromUserId = 41;
    outbound.toUserId = 42;
    outbound.ackRequested = true;

    WebSocketDispatcher dispatcher;
    WebSocketCodec codec(dispatcher);
    WsFrame frame;
    frame.opcode = WsOpcode::Text;
    frame.payload = WebSocketCodec::serializeApplicationMessage(outbound);
    const auto inbound = codec.messageFromFrame(frame);

    EXPECT_EQ(inbound.version, 2U);
    EXPECT_EQ(inbound.type, "chat");
    EXPECT_EQ(inbound.messageId, outbound.messageId);
    EXPECT_EQ(inbound.replyTo, outbound.replyTo);
    EXPECT_EQ(inbound.status, "accepted");
    EXPECT_EQ(inbound.text, outbound.text);
    EXPECT_EQ(inbound.fromUserId, 41U);
    EXPECT_EQ(inbound.toUserId, 42U);
    EXPECT_TRUE(inbound.ackRequested);

    frame.payload =
        "{\"type\":\"chat\",\"id\":\"unicode\","
        "\"to\":42,\"content\":\"\\u4f60\\u597d\"}";
    const auto escapedUnicode = codec.messageFromFrame(frame);
    EXPECT_EQ(escapedUnicode.text, std::string("\xE4\xBD\xA0\xE5\xA5\xBD"));

    frame.payload = "{\"type\":\"chat\",\"type\":\"ack\"}";
    EXPECT_EQ(codec.messageFromFrame(frame).type, "echo");
    frame.payload = "{\"type\":\"chat\"} trailing";
    EXPECT_EQ(codec.messageFromFrame(frame).type, "echo");
    frame.payload = "{\"type\":\"chat\"";
    EXPECT_EQ(codec.messageFromFrame(frame).type, "echo");
    frame.payload = "{\"type\":\"chat\",}";
    EXPECT_EQ(codec.messageFromFrame(frame).type, "echo");
}

/**
 * @test WebSocketDeliveryTrackerTest.DeduplicatesAndValidatesAckOwner
 * @brief 同一发送者重用 client id 时只能重复同一请求，且只有真实收件人能 ACK。
 */
TEST(WebSocketDeliveryTrackerTest, DeduplicatesAndValidatesAckOwner)
{
    WebSocketDeliveryTracker tracker;
    const auto now = WebSocketDeliveryTracker::TimePoint{};

    const auto created = tracker.begin(10, 20, "client-1", "hello", now);
    ASSERT_EQ(created.status, DeliveryBeginStatus::Created);
    EXPECT_FALSE(created.serverMessageId.empty());
    EXPECT_EQ(created.state, DeliveryState::AwaitingAck);

    const auto duplicate = tracker.begin(10, 20, "client-1", "hello", now);
    EXPECT_EQ(duplicate.status, DeliveryBeginStatus::Duplicate);
    EXPECT_EQ(duplicate.serverMessageId, created.serverMessageId);
    EXPECT_EQ(tracker.recordCount(), 1U);

    const auto conflict = tracker.begin(10, 21, "client-1", "changed", now);
    EXPECT_EQ(conflict.status, DeliveryBeginStatus::Conflict);

    const auto forged = tracker.acknowledge(99, created.serverMessageId, now);
    EXPECT_EQ(forged.status, DeliveryAckStatus::WrongRecipient);
    EXPECT_EQ(tracker.pendingCount(), 1U);

    const auto accepted = tracker.acknowledge(20, created.serverMessageId, now);
    EXPECT_EQ(accepted.status, DeliveryAckStatus::Acknowledged);
    EXPECT_EQ(accepted.sender, 10U);
    EXPECT_EQ(accepted.recipient, 20U);
    EXPECT_EQ(accepted.clientMessageId, "client-1");
    EXPECT_EQ(tracker.pendingCount(), 0U);

    const auto duplicateAck = tracker.acknowledge(20, created.serverMessageId, now);
    EXPECT_EQ(duplicateAck.status, DeliveryAckStatus::Duplicate);
}

/**
 * @test WebSocketDeliveryTrackerTest.BoundsWindowAndPrunesTerminalRecords
 * @brief 终态在保留期内继续承担去重，期满后释放容量。
 */
TEST(WebSocketDeliveryTrackerTest, BoundsWindowAndPrunesTerminalRecords)
{
    WebSocketDeliveryConfig config;
    config.maxRecords = 1;
    config.terminalRetention = std::chrono::milliseconds{10};
    WebSocketDeliveryTracker tracker(config);
    const auto start = WebSocketDeliveryTracker::TimePoint{};

    const auto first = tracker.begin(1, 2, "first", "one", start);
    ASSERT_EQ(first.status, DeliveryBeginStatus::Created);
    ASSERT_EQ(
        tracker.acknowledge(2, first.serverMessageId, start).status,
        DeliveryAckStatus::Acknowledged);

    EXPECT_EQ(
        tracker.begin(1, 2, "second", "two",
                      start + std::chrono::milliseconds{9}).status,
        DeliveryBeginStatus::Capacity);
    EXPECT_EQ(
        tracker.begin(1, 2, "second", "two",
                      start + std::chrono::milliseconds{10}).status,
        DeliveryBeginStatus::Created);
    EXPECT_EQ(tracker.recordCount(), 1U);
}

TEST(WebSocketDeliveryTrackerTest, KeepsClientAndServerIdLimitsIndependent)
{
    WebSocketDeliveryConfig config;
    config.maxClientMessageIdBytes = 1;
    WebSocketDeliveryTracker tracker(config);
    const auto now = WebSocketDeliveryTracker::TimePoint{};

    const auto created = tracker.begin(1, 2, "x", "payload", now);
    ASSERT_EQ(created.status, DeliveryBeginStatus::Created);
    EXPECT_EQ(
        tracker.acknowledge(2, created.serverMessageId, now).status,
        DeliveryAckStatus::Acknowledged);
    EXPECT_EQ(
        tracker.begin(1, 2, "too-long", "payload", now).status,
        DeliveryBeginStatus::Invalid);
}

/**
 * @test WebSocketDeliveryTrackerTest.EmitsBoundedRetryDecisionsThenFails
 * @brief maxAttempts 包含首次投递；三次上限意味着首次发送加两次重发。
 */
TEST(WebSocketDeliveryTrackerTest, EmitsBoundedRetryDecisionsThenFails)
{
    WebSocketDeliveryConfig config;
    config.maxAttempts = 3;
    config.ackTimeout = std::chrono::milliseconds{10};
    WebSocketDeliveryTracker tracker(config);
    const auto start = WebSocketDeliveryTracker::TimePoint{};

    const auto created = tracker.begin(7, 8, "client-7", "payload", start);
    ASSERT_EQ(created.status, DeliveryBeginStatus::Created);

    const auto early = tracker.collectDue(start + std::chrono::milliseconds{9});
    EXPECT_TRUE(early.retries.empty());
    EXPECT_TRUE(early.failures.empty());

    const auto second = tracker.collectDue(start + std::chrono::milliseconds{10});
    ASSERT_EQ(second.retries.size(), 1U);
    EXPECT_EQ(second.retries[0].attempt, 2U);
    EXPECT_EQ(second.retries[0].serverMessageId, created.serverMessageId);

    const auto third = tracker.collectDue(start + std::chrono::milliseconds{20});
    ASSERT_EQ(third.retries.size(), 1U);
    EXPECT_EQ(third.retries[0].attempt, 3U);

    const auto exhausted = tracker.collectDue(start + std::chrono::milliseconds{30});
    EXPECT_TRUE(exhausted.retries.empty());
    ASSERT_EQ(exhausted.failures.size(), 1U);
    EXPECT_EQ(exhausted.failures[0].serverMessageId, created.serverMessageId);
    EXPECT_EQ(tracker.pendingCount(), 0U);
    EXPECT_EQ(
        tracker.acknowledge(8, created.serverMessageId,
                            start + std::chrono::milliseconds{30}).status,
        DeliveryAckStatus::TooLate);
}

/**
 * @test WebSocketDispatcherTest.RoutesByTypeAndEchoDefault
 * @brief 验证 WebSocket 消息按 type 字段路由，未匹配时走默认 echo。
 *
 * 测试步骤：
 *   1. 注册 "chat" 类型的处理器，验证能收到消息
 *   2. 注册 onDefault 处理器，将未匹配消息原样回显为 "echo" 类型
 *   3. 发送 "@1002:hello" 帧，验证 messageFromFrame 解析出 type="chat"
 *   4. 验证 chat 处理器被调用，且 toUserId=1002, text="hello"
 *   5. 发送 "ping" 帧（无 @ 前缀），验证 type="echo"
 *   6. 验证 onDefault 被调用，且编码后的 outbound 与 encodeText("ping") 一致
 *
 * 为什么重要：
 *   - WebSocket 业务层路由按消息 type 字段分发，类似 HTTP 的 path 路由
 *   - "@uid:text" 是简写协议：@ 前缀表示点对点聊天消息，由 messageFromFrame 解析
 *   - onDefault 兜底确保未识别消息类型也有响应，不会静默丢弃
 */
TEST(WebSocketDispatcherTest, RoutesByTypeAndEchoDefault)
{
    WebSocketDispatcher dispatcher;
    bool chatCalled = false;
    dispatcher.on("chat", [&](WsMessageContext &ctx) -> bool
                   {
        chatCalled = true;
        EXPECT_EQ(ctx.inbound.toUserId, 1002u);
        EXPECT_EQ(ctx.inbound.text, "hello");
        return true;
    });
    dispatcher.onDefault([&](WsMessageContext &ctx) -> bool
                         {
        ctx.outbound = ctx.inbound;
        ctx.outbound.type = "echo";
        ctx.hasOutbound = true;
        return true;
    });

    WebSocketCodec codec(dispatcher);
    WsFrame frame;
    frame.opcode = WsOpcode::Text;
    frame.payload = "@1002:hello";
    auto msg = codec.messageFromFrame(frame);
    EXPECT_EQ(msg.type, "chat");

    WsMessageContext ctx;
    ctx.inbound = msg;
    EXPECT_TRUE(codec.dispatch(ctx));
    EXPECT_TRUE(chatCalled);
    EXPECT_FALSE(ctx.hasOutbound);

    frame.payload = "ping";
    msg = codec.messageFromFrame(frame);
    EXPECT_EQ(msg.type, "echo");
    ctx = WsMessageContext{};
    ctx.inbound = msg;
    EXPECT_TRUE(codec.dispatch(ctx));
    EXPECT_TRUE(ctx.hasOutbound);
    EXPECT_EQ(codec.encode(ctx.outbound), WebSocketCodec::encodeText("ping"));
}

/**
 * @test RequestContextTest.AcceptWebSocketFlag
 * @brief 验证 RequestContext::acceptWebSocket() 标志位。
 *
 * 测试步骤：
 *   1. 创建 RequestContext，初始 webSocketAccepted 应为 false
 *   2. 调用 ctx.acceptWebSocket()
 *   3. 验证 webSocketAccepted 变为 true
 *
 * 为什么重要：
 *   - acceptWebSocket() 是 /ws 路由 handler 告知框架"此连接要升级为 WebSocket"的标志
 *   - 框架在 handler 返回后检查此标志：若为 true 则走 WebSocket 握手流程
 *   - 标志位是 HTTP→WebSocket 协议切换的"信号灯"，必须可靠
 *   - 这是 main.cpp 中 /ws 路由调用的核心 API 的契约测试
 */
TEST(RequestContextTest, AcceptWebSocketFlag)
{
    RequestContext ctx;
    EXPECT_FALSE(ctx.webSocketAccepted);
    ctx.acceptWebSocket();
    EXPECT_TRUE(ctx.webSocketAccepted);
}

/**
 * @test WebSocketSessionManagerTest.OldConnectionCannotUnregisterReplacement
 * @brief 验证同 uid 重连时，旧连接不能误删新连接的目录项。
 *
 * 房号 fd 可能被复用，uid 也会被新登录覆盖；Manager 必须比较完整的
 * ConnectionKey。只有当前目录项仍属于发起注销的连接时，erase 才能发生。
 */
TEST(WebSocketSessionManagerTest, OldConnectionCannotUnregisterReplacement)
{
    WebSocketDispatcher dispatcher;
    WebSocketSessionManager manager;
    constexpr UserId uid = 42;
    const ConnectionKey oldKey{17, 1001};
    const ConnectionKey newKey{17, 1002};

    auto oldSession = std::make_shared<WebSocketSession>(
        oldKey, nullptr, uid, &manager, dispatcher);
    auto newSession = std::make_shared<WebSocketSession>(
        newKey, nullptr, uid, &manager, dispatcher);

    EXPECT_FALSE(manager.registerSession(uid, oldSession, 0, {}));
    ASSERT_TRUE(manager.registerSession(
        uid, oldSession, 0, oldKey));
    ASSERT_TRUE(manager.registerSession(
        uid, newSession, 0, newKey));

    EXPECT_FALSE(manager.unregister(uid, oldKey));
    EXPECT_EQ(manager.onlineCount(), 1U);
    EXPECT_TRUE(manager.unregister(uid, newKey));
    EXPECT_EQ(manager.onlineCount(), 0U);
}

TEST(WebSocketSessionManagerTest, PreservesOutboundRejectionReason)
{
    WebSocketSessionManager manager;
    EXPECT_EQ(manager.sendText(42, "hello"), EnqueueResult::Closed);
    EXPECT_EQ(
        manager.sendText(42, std::string("\xC0\xAF", 2)),
        EnqueueResult::Invalid);

    const auto offline = manager.sendTextTracked(42, "hello");
    ASSERT_TRUE(offline.receipt);
    EXPECT_EQ(offline.admission, EnqueueResult::Closed);
    EXPECT_EQ(offline.receipt->outcome(), OutboundOutcome::Closed);

    const auto invalid =
        manager.sendTextTracked(42, std::string("\xC0\xAF", 2));
    ASSERT_TRUE(invalid.receipt);
    EXPECT_EQ(invalid.admission, EnqueueResult::Invalid);
    EXPECT_EQ(invalid.receipt->outcome(), OutboundOutcome::Invalid);
}
