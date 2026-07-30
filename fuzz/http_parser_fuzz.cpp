// HTTP 解析器的 libFuzzer 入口。
// 目标不是判断随机字节是否为合法 HTTP，而是让任意输入、任意分包方式都不会
// 触发越界、未定义行为、无限循环或状态残留；配合 Sanitizer 可持续扩展协议边界覆盖。
#include <cstddef>
#include <cstdint>

#include <algorithm>

#include "server/Buffer/Buffer.h"
#include "server/http/HttpParser/HttpParser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // 限制单个样本可控地占用内存和执行时间。上限高于解析器正常正文上限，
    // 仍能探索“超限后拒绝”的路径，而不会让模糊测试吞吐被巨型样本拖垮。
    constexpr size_t kMaxInput = 2 * 1024 * 1024;
    if (size == 0 || size > kMaxInput)
        return 0;

    HttpParser parser;
    HttpRequest request;
    Buffer buffer(64);

    // 用首字节派生固定分片步长，使同一输入可重复，同时让语料自然覆盖
    // 1～64 字节的网络分包边界。首字节仍作为报文数据送入，不牺牲输入空间。
    const size_t stride = 1 + (data[0] % 64);
    size_t offset = 0;
    while (offset < size)
    {
        const size_t amount = std::min(stride, size - offset);
        buffer.append(reinterpret_cast<const char*>(data + offset), amount);
        offset += amount;

        // 一次 append 可能含多个流水线请求，因此持续解析至需要更多数据。
        // 这样既覆盖单请求状态机，也验证 reset 后消费缓冲区中后续请求的能力。
        while (true)
        {
            // 记录解析前字节数，用于检测“返回成功但未消费输入”的非显然状态；
            // 若继续循环会形成 fuzzer 卡死，因此必须主动退出。
            const size_t before = buffer.readableBytes();
            const ParseState state = parser.parse(buffer, request);
            if (state == PARSE_ERROR)
                // 语法错误是随机输入的正常结果，不应被当作 fuzz 崩溃。
                return 0;
            if (state == PARSE_NEED_MORE)
                break;

            // 成功完成一个请求后清除请求级状态，模拟连接复用；剩余 Buffer
            // 保持不变，以继续探索 HTTP pipelining 和跨请求状态污染风险。
            parser.reset();
            request.reset();
            if (buffer.readableBytes() == 0 || buffer.readableBytes() == before)
                break;
        }
    }

    // 最后再解析一次可触达 EOF 前不完整报文的处理路径；返回值本身不是断言，
    // Sanitizer 所报告的内存安全和未定义行为才是该 fuzz target 的判定依据。
    (void)parser.parse(buffer, request);
    return 0;
}
