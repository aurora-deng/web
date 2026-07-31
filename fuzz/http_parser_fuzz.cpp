// HTTP 解析器的 libFuzzer 模糊测试入口函数
// 核心测试目标：
// 1. 不校验随机字节是否符合合法HTTP协议规范（随机输入大量非法报文属于正常场景）
// 2. 重点保障：任意输入内容、任意TCP分包方式下，解析器绝对不会触发内存越界、未定义行为、无限循环、跨请求状态残留问题
// 3. 配合AddressSanitizer/UBSan等内存检测工具，持续覆盖协议边界、异常分支、极限场景，挖掘隐蔽安全漏洞
#include <cstddef>
#include <cstdint>

#include <algorithm>

// 项目自定义环形缓冲区封装，用于模拟TCP分片接收的数据缓冲区
#include "server/Buffer/Buffer.h"
// 自研HTTP请求解析器核心状态机
#include "server/http/HttpParser/HttpParser.h"

// libFuzzer标准入口函数签名，由Fuzzing框架自动循环调用、变异输入样本
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // ========== 样本大小安全限制 ==========
    // 单份测试样本内存上限：2MB
    // 设计考量：
    // 1. 上限略高于业务正常配置的HTTP正文最大值，可覆盖"超长报文触发超限拒绝"的异常分支
    // 2. 限制超大样本避免单次测试占用过多内存、执行过久，保障模糊测试整体迭代吞吐效率
    constexpr size_t kMaxInput = 2 * 1024 * 1024;
    // 空样本直接跳过；超上限样本直接丢弃，规避巨型样本拖慢测试流程
    if (size == 0 || size > kMaxInput)
        return 0;

    // 实例化解析器状态机（每个测试样本独立全新实例，避免样本间状态互相干扰）
    HttpParser parser;
    // 存储单次解析产出的HTTP请求结构化结果
    HttpRequest request;
    // 初始化缓冲区，初始容量64字节，模拟服务端Socket接收缓冲区
    Buffer buffer(64);

    // ========== 模拟TCP网络分片分包逻辑 ==========
    // 利用输入首字节计算分片步长stride：范围固定1~64字节
    // 优点：
    // 1. 同一输入样本永远生成相同分片策略，测试结果可复现，便于复现定位漏洞
    // 2. 天然覆盖1~64字节所有常见TCP分包边界，精准模拟网络上粘包、分包的真实场景
    // 3. 首字节完整写入缓冲区参与解析，不会浪费输入字节、减少测试覆盖范围
    const size_t stride = 1 + (data[0] % 64);
    // 当前已处理的输入偏移量，遍历完整输入数据
    size_t offset = 0;

    // 循环按分片大小逐段向缓冲区填充数据，模拟TCP分段接收报文
    while (offset < size)
    {
        // 本次实际填充字节数：取分片步长、剩余未读数据量二者较小值，防止越界读取输入
        const size_t amount = std::min(stride, size - offset);
        // 将分片数据追加到接收缓冲区，模拟内核Socket收到数据写入应用层缓冲区
        buffer.append(reinterpret_cast<const char*>(data + offset), amount);
        // 偏移量前进，处理下一段分片数据
        offset += amount;

        // ========== 循环解析缓冲区（支持HTTP流水线Pipeline场景） ==========
        // HTTP流水线特性：单个TCP连接可连续堆积多个完整请求，必须循环解析直到数据不足
        // 同时验证：单次append多条请求时，解析器能否逐个消费、无状态污染
        while (true)
        {
            // 记录解析执行前缓冲区可读字节数，用于关键防卡死校验
            // 极端bug场景：解析器返回非NEED_MORE状态，但完全未消费缓冲区数据，持续循环会直接造成fuzzer无限卡死
            const size_t before = buffer.readableBytes();
            // 调用解析器核心逻辑，从缓冲区解析填充request对象，返回解析状态枚举
            const ParseState state = parser.parse(buffer, request);

            // 解析返回语法错误：随机变异输入出现非法HTTP报文是预期正常情况，不属于漏洞
            // 直接结束当前样本测试，交由libFuzzer变异下一份样本继续测试
            if (state == PARSE_ERROR)
                return 0;

            // 返回需要更多数据：当前缓冲区报文不完整，退出内层循环继续填充下一分片数据
            if (state == PARSE_NEED_MORE)
                break;

            // 走到此处代表：成功解析完1条完整HTTP请求（PARSE_OK状态）
            // 重置解析器状态+请求结构体，模拟HTTP长连接复用场景：连接不断开，处理完一个请求立刻准备接收下一个
            // 缓冲区剩余未消费数据完整保留，专门测试跨请求状态残留、状态机污染的隐蔽问题
            parser.reset();
            request.reset();

            // 防卡死双重校验：
            // 1. 缓冲区已无剩余数据，无内容可继续解析，退出内层循环
            // 2. 解析前后可读字节数完全一致=本次解析未消费任何数据，继续循环必然卡死，强制退出
            if (buffer.readableBytes() == 0 || buffer.readableBytes() == before)
                break;
        }
    }

    // ========== 收尾边界场景覆盖 ==========
    // 所有分片数据填充完毕后再执行一次解析调用
    // 目的：覆盖TCP连接主动关闭、EOF到达时，缓冲区残留不完整报文的收尾处理逻辑
    // 不接收返回值做断言判断：模糊测试核心判定标准是Sanitizer工具捕获的内存安全/未定义行为问题，而非业务返回码
    (void)parser.parse(buffer, request);

    // 当前样本测试正常结束，libFuzzer自动变异生成下一份新样本持续迭代测试
    return 0;
}