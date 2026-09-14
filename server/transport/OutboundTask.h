// =============================================================================
// 文件名：OutboundTask.h
// 所属模块：server/transport —— 协议无关的"出站发货单"
//
// 【职责比喻：发货部的"统一运单"】
//   不管是 HTTP 响应还是 WebSocket 帧，只要要往 socket 上写，就统一打成一张 OutboundTask
//   运单塞进出站队列。运单里装的三种货：编好的字节串（EncodedBufferTask）、共享的只读
//   字节串（SharedEncodedBufferTask，多连接广播同一段内容时省拷贝）、待流式发送的
//   HttpResponse（HttpStreamTask，可走 sendfile 零拷贝）。每张运单带一张票据（ticket），
//   写完后回填 completedTicket，让等货的协程知道"这单发完了"。
//
// 关键技术点（初学者重点理解）：
//   1. 【协议无关】OutboundTask 不 include 任何协议头，只认字节串 / HttpResponse 抽象。
//      HTTP 与 WebSocket 的发送都走同一套队列与 Writer，是"协议无关出站体系"的基石。
//   2. 【variant 三态载荷】Payload = variant<EncodedBufferTask, SharedEncodedBufferTask,
//      HttpStreamTask>。encoded 走 writev 聚集写；http 走 sendfile/writev 混合。用 variant
//      而非继承，避免堆分配 + 虚函数分发，出站热路径零堆开销。
//   3. 【票据序号】ticket 是单调递增的发货流水号。协程 reserveOutboundTicket 领号 → 入队 →
//      co_await SendCompletionAwaiter 等该号完成；Writer 写完回填 completedTicket 唤醒。0
//      号保留给"无需等待"的任务（如主动关闭连接的尾包）。
//   4. 【完成动作 OutboundCompletion】None = 写完继续，CloseConnection = 写完立刻关连接。
//      让"发完最后一个响应就关"成为 OutboundTask 自带语义，Writer 检到就回调 fd_close。
//   5. 【PooledHttpResponse 池化 RAII】HttpResponse 对象走对象池（responsePool）复用，
//      PooledHttpResponse 是它的 RAII 包装：析构自动归还池，移动语义转移所有权，禁拷贝
//      防止双重释放。出站热路径不 new/delete，降低碎片。
//   6. 【跨层契约】Session 构造 OutboundTask 经 SubReactor 窄接口入队（不直接摸队列）；
//      OutboundQueue 管跨线程投递；TransportWriter 是唯一写 socket 入口。三层各司其职，
//      Session 永远不直接 write/writev/sendfile。
// =============================================================================
#pragma once
#ifndef OUTBOUND_TASK_H
#define OUTBOUND_TASK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include "server/transport/EnqueueResult.h"

struct HttpResponse;

/**
 * @brief HttpResponse 的池化 RAII 包装
 *
 * 【通俗解释】HttpResponse 对象很重（含头/体/分块结构），反复 new/delete 会产生碎片。
 *   用一个全局对象池（responsePool）循环复用：借出用 get()，用完析构自动归还。
 *   PooledHttpResponse 就是这张"借条"——持有它就拥有 response，析构即归还，移动即交接，
 *   禁拷贝防止一张借条变两张导致双重归还。
 *
 * @note 配合 OutboundTask::http() 使用：业务构造好响应后包成 PooledHttpResponse 入队，
 *       Writer 写完整个 task 析构时自动归还池，全程零 new/delete。
 */
class PooledHttpResponse
{
public:
    PooledHttpResponse() = default;
    explicit PooledHttpResponse(HttpResponse *response) : response_(response) {}
    ~PooledHttpResponse();  // 析构自动归还池：离开作用域即回收，RAII 核心

    // ---- 移动语义：转移借条所有权，原对象置空 ----
    PooledHttpResponse(PooledHttpResponse &&other) noexcept;
    PooledHttpResponse &operator=(PooledHttpResponse &&other) noexcept;

    // ---- 禁拷贝：池化对象唯一归属，拷贝会导致双重归还 ----
    PooledHttpResponse(const PooledHttpResponse &) = delete;
    PooledHttpResponse &operator=(const PooledHttpResponse &) = delete;

    HttpResponse *get() const { return response_; }      // 借用：不转移所有权
    HttpResponse *release();                              // 放弃所有权并交出裸指针（调用方负责归还）
    explicit operator bool() const { return response_ != nullptr; } // 是否持有一张有效借条

private:
    void reset();  // 内部归还：把 response_ 还给 responsePool 并置空
    HttpResponse *response_ = nullptr;
};

/**
 * @brief 载荷变体①：独占字节串（已编码好的待发字节）
 *
 * 【通俗解释】自己独占一串字节（bytes），offset 记已写偏移。适合一次性响应。
 */
struct EncodedBufferTask
{
    std::string bytes;       // 已编码的完整字节串
    std::size_t offset = 0;  // 已写入 socket 的偏移；writev 部分写后回填，下次续写
};

/**
 * @brief 载荷变体②：共享只读字节串（多连接广播复用）
 *
 * 【通俗解释】多个连接要发同一段内容（如广播消息）时，用 shared_ptr 共享同一份只读
 *   字节串，省去每连接拷贝。各连接各自的 offset 独立推进。
 */
struct SharedEncodedBufferTask
{
    std::shared_ptr<const std::string> bytes;  // 共享的只读字节串
    std::size_t offset = 0;                    // 本连接的写入偏移
};

/**
 * @brief 载荷变体③：待流式发送的 HttpResponse（可走 sendfile 零拷贝）
 *
 * 【通俗解释】装一个池化的 HttpResponse，由 Writer 按 HeaderBody + body 分段发送；
 *   body 若是文件可走 sendfile 零拷贝，是静态文件响应的高性能路径。
 */
struct HttpStreamTask
{
    PooledHttpResponse response;  // 池化响应对象，写完析构自动归还
};

/**
 * @brief 出站任务完成后的附加动作
 *
 * 【通俗解释】None = 写完继续守连接（Keep-Alive）；CloseConnection = 写完立刻关连接
 *   （如非 Keep-Alive 响应、错误页、WebSocket Close 帧）。让"发完即关"成为运单自带语义。
 */
enum class OutboundCompletion
{
    None,
    CloseConnection
};

/**
 * @brief 一张出站任务最终停在什么位置
 *
 * 【快递回执 通俗解释】EnqueueResult 只是“快递站是否收件”，OutboundOutcome 才是
 * “包裹最后怎样了”。Written 只保证整张任务的字节已交给本机内核 socket 缓冲区，
 * 不等于对端应用已经读取；其余状态说明任务在某一层被拒绝或取消。
 */
enum class OutboundOutcome : std::uint8_t
{
    Pending,
    Written,
    Backpressured,
    Closed,
    Stale,
    Invalid,
    WriteError
};

/**
 * @brief 可跨线程保存的发送回执；终态只允许从 Pending 写入一次
 *
 * 只保存原子状态，不直接执行用户回调，避免 Writer 热路径发生重入或被慢回调阻塞。
 */
class OutboundReceipt
{
public:
    OutboundOutcome outcome() const noexcept
    {
        return outcome_.load(std::memory_order_acquire);
    }

    bool complete(OutboundOutcome outcome) noexcept
    {
        if (outcome == OutboundOutcome::Pending)
            return false;
        auto expected = OutboundOutcome::Pending;
        return outcome_.compare_exchange_strong(
            expected,
            outcome,
            std::memory_order_release,
            std::memory_order_relaxed);
    }

private:
    std::atomic<OutboundOutcome> outcome_{OutboundOutcome::Pending};
};

struct OutboundSubmission
{
    EnqueueResult admission = EnqueueResult::Invalid;
    std::shared_ptr<OutboundReceipt> receipt;
};

/**
 * @brief 出站任务：协议无关的"发货单"
 *
 * 【发货单 通俗解释】
 *   一张发货单三要素：货（payload，variant 三态）、流水号（ticket）、发完动作（completion）。
 *   Session 打单 → SubReactor 入队 → TransportWriter 取单发货 → 写完按 ticket 回填。
 *   全程协议无关，HTTP 与 WebSocket 共用同一套发送管线。
 *
 * 【为何只 move 不 copy 通俗解释】
 *   发货单里可能装着重货（HttpResponse / 大字节串），拷贝代价高且语义混乱（两张单发同一份货
 *   会导致票据/归还不一致）。只允许移动：单子从 Session 手里转到队列，所有权清晰，零拷贝。
 *
 * @note 工厂方法 encoded() / http() / sharedEncoded() 是构造入口，构造函数私有强制走工厂，
 *       保证 ticket / completion 默认值正确。
 */
class OutboundTask
{
public:
    using Payload =
        std::variant<EncodedBufferTask, SharedEncodedBufferTask, HttpStreamTask>;

    // ---- 三个工厂方法：按载荷类型构造，统一填默认 ticket/completion ----
    static OutboundTask encoded(std::string bytes,
                                uint64_t ticket = 0,
                                OutboundCompletion completion = OutboundCompletion::None,
                                std::shared_ptr<OutboundReceipt> receipt = {});
    static OutboundTask http(PooledHttpResponse response,
                             uint64_t ticket,
                             OutboundCompletion completion = OutboundCompletion::None,
                             std::shared_ptr<OutboundReceipt> receipt = {});
    static OutboundTask sharedEncoded(
        std::shared_ptr<const std::string> bytes,
        uint64_t ticket = 0,
        OutboundCompletion completion = OutboundCompletion::None,
        std::shared_ptr<OutboundReceipt> receipt = {});

    // ---- 只 move 不 copy：载荷可能含重资源，移动转移所有权，拷贝禁止 ----
    OutboundTask(OutboundTask &&other) noexcept;
    OutboundTask &operator=(OutboundTask &&other) noexcept;
    ~OutboundTask();
    OutboundTask(const OutboundTask &) = delete;
    OutboundTask &operator=(const OutboundTask &) = delete;

    std::size_t remainingBytes() const;  // 本单还剩多少字节未写（背压计数用）
    void complete(OutboundOutcome outcome) noexcept;

    Payload payload;                                     // 载荷（variant 三态）
    uint64_t ticket = 0;                                 // 流水号；0 表示无需等待完成
    OutboundCompletion completion = OutboundCompletion::None; // 发完动作
    std::shared_ptr<OutboundReceipt> receipt;             // 可选最终回执；普通热路径可不创建

private:
    OutboundTask(Payload payload,
                 uint64_t ticket,
                 OutboundCompletion completion,
                 std::shared_ptr<OutboundReceipt> receipt);
};

#endif
