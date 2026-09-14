// =============================================================================
// 文件名：OutboundTask.cpp
// 所属模块：server/transport —— OutboundTask 出站发货单的实现
//
// 【职责比喻：发货单的"装订车间"】
//   本文件实现 PooledHttpResponse 的池化 RAII（借/还/转移）与 OutboundTask 的三个工厂方法
//   （encoded / http / sharedEncoded）及 remainingBytes() 载荷剩余量查询。把"如何打单"
//   的细节集中在这里，头文件只留接口。
//
// 关键技术点（初学者重点理解）：
//   1. 【对象池复用】PooledHttpResponse 的 reset() 把 response 归还全局 responsePool，
//      析构即回收，避免热路径 new/delete，降低内存碎片。
//   2. 【移动语义实现】移动构造/赋值用 release() 转移裸指针所有权，原对象置空，确保
//      "一张借条只有一个主人"，析构不会双重归还。
//   3. 【variant 访问】remainingBytes() 用 std::get_if 按载荷类型取剩余字节；HttpStreamTask
//      分 HeaderBody + body 两段求和，支持 sendfile 与内存体混合。
// =============================================================================
#include "OutboundTask.h"

#include "server/ObjectPool/ObjectPool.h"
#include "server/http/http.h"

#include <utility>

extern ObjectPoll<HttpResponse> responsePool;  // 全局响应对象池，PooledHttpResponse 借还的目标

// ---- PooledHttpResponse：池化 RAII 实现 ----

PooledHttpResponse::~PooledHttpResponse()
{
    reset();  // 析构归还：持有 response 则还池，否则空操作
}

// 移动构造：从 other 借出裸指针（release 顺手把 other 置空），本对象接管所有权
PooledHttpResponse::PooledHttpResponse(PooledHttpResponse &&other) noexcept
    : response_(other.release())
{
}

PooledHttpResponse &PooledHttpResponse::operator=(PooledHttpResponse &&other) noexcept
{
    if (this != &other)  // 自赋值防护
    {
        reset();                    // 先归还自己手里的旧 response
        response_ = other.release(); // 再接手 other 的（release 把 other 置空）
    }
    return *this;
}

HttpResponse *PooledHttpResponse::release()
{
    auto *response = response_;  // 放弃所有权，交出裸指针
    response_ = nullptr;         // 本对象置空，析构时不再归还
    return response;
}

void PooledHttpResponse::reset()
{
    if (response_)  // 只有真持有 response 才归还，避免空归还
    {
        responsePool.release(response_);  // 还池：交给 ObjectPoll 复用
        response_ = nullptr;
    }
}

// ---- OutboundTask：私有构造 + 三个工厂方法 ----

OutboundTask::OutboundTask(Payload value,
                           uint64_t taskTicket,
                           OutboundCompletion taskCompletion,
                           std::shared_ptr<OutboundReceipt> taskReceipt)
    : payload(std::move(value)),
      ticket(taskTicket),
      completion(taskCompletion),
      receipt(std::move(taskReceipt))
{
}

OutboundTask::OutboundTask(OutboundTask &&other) noexcept
    : payload(std::move(other.payload)),
      ticket(other.ticket),
      completion(other.completion),
      receipt(std::move(other.receipt))
{
}

OutboundTask &OutboundTask::operator=(OutboundTask &&other) noexcept
{
    if (this != &other)
    {
        // 覆盖一张仍在途的旧单前先收尾，避免它的回执永久 Pending。
        complete(OutboundOutcome::Closed);
        payload = std::move(other.payload);
        ticket = other.ticket;
        completion = other.completion;
        receipt = std::move(other.receipt);
    }
    return *this;
}

OutboundTask::~OutboundTask()
{
    // 任何未经过显式完成路径就被销毁的任务，都留下 Closed，而不是永远卡在 Pending。
    complete(OutboundOutcome::Closed);
}

void OutboundTask::complete(OutboundOutcome outcome) noexcept
{
    if (receipt)
        receipt->complete(outcome);
}

// 工厂①：独占字节串任务，offset 起始 0，由 Writer 边写边推进
OutboundTask OutboundTask::encoded(std::string bytes,
                                   uint64_t ticket,
                                   OutboundCompletion completion,
                                   std::shared_ptr<OutboundReceipt> receipt)
{
    return OutboundTask{
        EncodedBufferTask{std::move(bytes), 0},
        ticket,
        completion,
        std::move(receipt)};
}

// 工厂③：HttpStreamTask，装池化响应；sendfile/writev 由 TransportWriter 决定
OutboundTask OutboundTask::http(PooledHttpResponse response,
                                uint64_t ticket,
                                OutboundCompletion completion,
                                std::shared_ptr<OutboundReceipt> receipt)
{
    return OutboundTask{
        HttpStreamTask{std::move(response)},
        ticket,
        completion,
        std::move(receipt)};
}

// 工厂②：共享只读字节串任务，多连接广播同一段内容时省拷贝
OutboundTask OutboundTask::sharedEncoded(
    std::shared_ptr<const std::string> bytes,
    uint64_t ticket,
    OutboundCompletion completion,
    std::shared_ptr<OutboundReceipt> receipt)
{
    return OutboundTask{
        SharedEncodedBufferTask{std::move(bytes), 0},
        ticket,
        completion,
        std::move(receipt)};
}

// 查本单还剩多少字节未写：按载荷类型分流，供背压计数（pendingWriteBytes）使用
std::size_t OutboundTask::remainingBytes() const
{
    // 独占字节串：总长 - 已写偏移
    if (const auto *encoded = std::get_if<EncodedBufferTask>(&payload))
        return encoded->bytes.size() - encoded->offset;
    // 共享字节串：同上，但 bytes 可能为空指针需防护
    if (const auto *encoded = std::get_if<SharedEncodedBufferTask>(&payload))
        return encoded->bytes
                   ? encoded->bytes->size() - encoded->offset
                   : 0;

    // Http 响应：HeaderBody 剩余 + body 剩余两段求和
    const auto &http = std::get<HttpStreamTask>(payload);
    const auto *response = http.response.get();
    if (!response)
        return 0;

    std::size_t bytes = 0;
    if (response->HeaderBody_)             // 头部分段剩余
        bytes += response->HeaderBody_->remain();
    if (response->body)                    // 体部剩余（可能是文件/内存/分块）
        bytes += response->body->remain();
    return bytes;
}
