// =============================================================================
// 文件名：ChunkedBody.h
// 职责比喻：分块传输响应体 —— 边生成边发的"流水线包装盒"
//
// 【整体比喻】
// 有时候服务端在生成响应时不知道总长度（比如实时计算的数据、SSE 推送、大数据查询流式返回）。
// HTTP 的 chunked 传输编码就是为此而生：把数据切成一块一块发送，每块前面标明本块长度，
// 最后发一个长度为 0 的块表示结束。就像工厂流水线：产品一个个生产出来就立刻装箱发货，
// 不需要等全部生产完再一次性发。
//
// 【chunk 格式】每块格式为：
//   "块长度(十六进制)\r\n" + 块数据 + "\r\n"
// 最后块：
//   "0\r\n\r\n"
//
// 【StreamQueue 多生产单消费】
// 业务线程可以随时 push 数据块进队列，发送协程从队列取出发送。
// 用 shared_ptr<StreamQueue> 让生产者和消费者共享同一个队列，
// 队列空时消费者挂起等待 wakeup 回调唤醒。
//
// 关键技术点（初学者重点理解）：
//   1. 多生产单消费：业务线程 push，单个发送协程 consume，deque + mutex 保证安全。
//   2. wakeup 回调：队列从空变非空时唤醒挂起的发送协程，避免空轮询。
//   3. 三段式块结构：每块由 prefix(长度行) + data(数据) + suffix(\r\n) 组成，
//      发送时可能只发出一部分，用 sent 字段记录每块发到哪。
//   4. finish 标记结束：push 一个 "0\r\n\r\n" 终止块后置 done=true，发送完即结束。
// =============================================================================
#ifndef CHUNKED_BODY_H
#define CHUNKED_BODY_H
#include <deque>
#include <functional>
#include <sys/uio.h>
#include <algorithm>
#include <memory>
#include <utility>

#include "RespBody.h"
#include "StringBody.h"
#include "server/Buffer/Buffer.h"
#include "server/SegmentPool/SegmentPool.h"

// =============================================================================
// ChunkBolck：一个分块的三段式结构
// =============================================================================
// 【ChunkBolck 通俗解释】
// 一个 HTTP chunk 在内存里被拆成三段：
//   prefix : 形如 "1a\r\n" 的长度行（十六进制长度 + CRLF）
//   data   : 实际数据内容（用 StringBody 持有，借 Buffer 池）
//   suffix : 固定 "\r\n"（块结束标记）
// sent 字段记录这三段累计已发送字节数，用于部分发送后续传。
// =============================================================================
struct ChunkBolck
{
    std::string prefix;                          // 长度行：" hex_len \r\n"
    std::shared_ptr<StringBody> data = nullptr;  // 数据体（可空，如终止块）
    std::string suffix = "\r\n";                 // 块结束 CRLF
    size_t sent = 0;                             // 本块已发送字节数
};

// =============================================================================
// StreamQueue：多生产单消费的分块队列
// =============================================================================
// 【StreamQueue 通俗解释】
// 这是一个线程安全队列，业务线程往里 push 块，发送协程从里取块发。
// 像餐厅的"传菜窗口"：厨师（业务线程）做好菜放窗口，服务员（发送协程）从窗口取菜上桌。
// 队列空时服务员可以"挂起等待"，厨师放菜时通过 wakeup 回调叫醒服务员。
// =============================================================================
struct StreamQueue
{
    std::mutex mtx;                  // 保护 chunks 的互斥锁
    std::deque<ChunkBolck> chunks;   // 待发送的分块队列（deque 支持头尾高效操作）

    bool finished = false;           // 是否已 push 终止块

    /**
     * @brief 向队列追加一个分块（生产端）
     * @param c 待发送的分块
     * 加锁 push 后，若注册了 wakeup 回调则调用，唤醒等待的消费者
     * 【设计动机】故意对于push来写一个pushchunk是为了后续扩展方便，有利于集成一些策略
     */
    void pushChunk(ChunkBolck c);

    // 增加回调函数使得自己唤醒自己
    std::function<void()> wakeup;    // 队列从空变非空时的唤醒回调
};
using StreamQueuePtr = std::shared_ptr<StreamQueue>;

// =============================================================================
// ChunkedBody：分块传输响应体，RespBody 的流式实现
// =============================================================================
// 【ChunkedBody 通俗解释】
// ChunkedBody 内部持有一个 StreamQueue，业务方调 push() 追加分块，发送器调 buildSegments()
// 取出队首块的三段（prefix/data/suffix）生成 Segment。consume() 按已发送字节推进各块的 sent，
// 一块发完就 pop 出队列。finish() push 终止块标记结束。
//
// 由于使用的是共享指针，所以依旧使用通过共享指针的数量来判断是否使用 chunk
// =============================================================================
class ChunkedBody : public RespBody
{
public:
    ~ChunkedBody() override = default;
    // bool next(std::vector<Segment>&seg,size_t max) override;
    int buildSegments(Block* block,size_t max) override;
    // bool buildIov(std::vector<iovec> &vec, size_t maxBytes) override;
    void consume(size_t bytes) override;
    bool finished() const override;
    void finish() override;
    size_t remain() const override;
    ssize_t sendFile(int fd, size_t maxBytes) override;
    size_t memoryUsage() const override;
    bool Chunked() const override;

    /**
     * @brief 向队列追加一个分块（业务侧调用）
     * @param c 分块数据
     * 队列从空变非空时触发 wakeup 回调，唤醒可能挂起的发送协程
     */
    void push(ChunkBolck c);

    // 由于使用的是共享指针，所以依旧使用通过共享指针的数量来判断是否使用chunk
    std::function<void()> wakeup;                          // 队列唤醒回调
    StreamQueuePtr stream = std::make_shared<StreamQueue>();  // 共享的块队列

private:
    size_t chunkIndex = 0;   // 已处理的块计数（调试/统计用）
    bool done = false;       // 是否已 push 终止块
};

#endif
