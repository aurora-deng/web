// =============================================================================
// 文件名：HeaderBody.h
// 职责比喻：响应头响应体 —— HTTP 响应头的"信封外壳"
//
// 【整体比喻】
// HTTP 响应由两部分组成：响应头（状态行 + 各种头字段）和响应体（实际数据）。
// HeaderBody 专门用来承载"响应头"这部分内容——它把序列化好的头部字符串装进 Buffer，
// 发送时像 StringBody 一样借用 Buffer 地址生成 Segment。
//
// 【为什么单独做一个 Body 类型？】
// TransportWriter 统一用 RespBody 接口发送数据，响应头也要走这套流程。
// 把头部字符串包成 HeaderBody，就能和 StringBody/FileBody 等响应体一起塞进发送队列，
// 实现"先发头、后发体"的顺序发送，复用同一套段式发送逻辑。
//
// 关键技术点（初学者重点理解）：
//   1. 与 StringBody 结构几乎相同：Buffer + offset，区别在于语义（这是头部不是体）。
//   2. append 累加：响应头通常分多行 append 进来（状态行、Content-Type、Content-Length...）。
//   3. BUFFER 类型段：标记为 BUFFER，发送器知道这是普通内存数据。
// =============================================================================
#ifndef HEADER_H
#define HEADER_H
#include <vector>
#include <sys/uio.h>
#include <algorithm>
#include <memory>

#include "RespBody.h"
#include "server/Buffer/Buffer.h"
#include "server/buffer_pool/BufferPoll.h"
#include "server/SegmentPool/SegmentPool.h"

// =============================================================================
// HeaderBody：HTTP 响应头响应体
// =============================================================================
// 【HeaderBody 通俗解释】
// 和 StringBody 一样持有 Buffer + offset，但语义上装的是 HTTP 响应头（而非响应体）。
// HttpResponse::buildHeader() 把状态行和所有头字段序列化成字符串，append 到 HeaderBody 的 Buffer，
// 然后 HeaderBody 作为发送队列的第一个 RespBody 先发出去，之后再发真正的响应体（StringBody 等）。
// =============================================================================
class HeaderBody : public RespBody
{
public:
    std::shared_ptr<Buffer> buffer_;   // 持有的头部数据缓冲区（池化）
    size_t offset = 0;                 // 已发送偏移，支持非阻塞续传

    /**
     * @brief 默认构造：从 BufferPoll 申请空 Buffer
     */
    explicit HeaderBody()
    {
        buffer_ = BufferPoll::instance().acquire();
    }
    // 新增：接收 shared_ptr<Buffer> 的构造函数
    /**
     * @brief 接管外部已填充好的 Buffer
     * @param buf 外部已写入头部数据的 Buffer
     * 【设计动机】避免重复拷贝，直接复用外部已构造好的 Buffer
     */
    explicit HeaderBody(std::shared_ptr<Buffer> buf)
    {
        buffer_ = std::move(buf);
    }

    /**
     * @brief 析构：归还 Buffer 给对象池
     */
    ~HeaderBody();

    /**
     * @brief 追加头部字符串到 Buffer
     * @param s 头部片段（如 "Content-Type: text/html\r\n"）
     * 多次 append 拼接出完整响应头
     */
    void append(const std::string &s);

    int buildSegments(Block *block, size_t max) override;
    void consume(size_t bytes) override;
    bool finished() const override;
    size_t remain() const override;
    ssize_t sendFile(int fd, size_t maxBytes) override;
    size_t memoryUsage() const override;
};

#endif
