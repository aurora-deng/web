// =============================================================================
// WebSocketDeliveryTracker：应用层消息 ID、ACK 归属、幂等与重试决策状态机
//
// 【挂号信登记簿】WebSocket/TCP 只负责搬字节；业务要知道“哪封信被谁确认”，必须另建
// 登记簿。Tracker 不写 socket、不起线程，只保存纯状态：业务层可在任意定时器中调用
// collectDue() 取得应该重发或宣告失败的动作。
// =============================================================================
#pragma once
#ifndef WEBSOCKET_DELIVERY_TRACKER_H
#define WEBSOCKET_DELIVERY_TRACKER_H

#include "server/websocket/WebSocketTypes/UserId.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

enum class DeliveryState
{
    AwaitingTransport,
    AwaitingAck,
    RetryScheduled,
    Acknowledged,
    Failed
};

enum class DeliveryBeginStatus
{
    Created,
    Duplicate,
    Conflict,
    Capacity,
    Invalid
};

struct DeliveryBeginResult
{
    DeliveryBeginStatus status = DeliveryBeginStatus::Invalid;
    std::string serverMessageId;
    DeliveryState state = DeliveryState::Failed;
};

enum class DeliveryAckStatus
{
    Acknowledged,
    Duplicate,
    Unknown,
    WrongRecipient,
    TooLate,
    Invalid
};

struct DeliveryAckResult
{
    DeliveryAckStatus status = DeliveryAckStatus::Invalid;
    std::string serverMessageId;
    std::string clientMessageId;
    UserId sender = 0;
    UserId recipient = 0;
};

enum class DeliveryAttemptOutcome
{
    Written,
    RetryableFailure,
    PermanentFailure
};

enum class DeliveryAttemptUpdateStatus
{
    Applied,
    StaleAttempt,
    Unknown,
    Terminal,
    Invalid
};

struct DeliveryAttemptUpdate
{
    DeliveryAttemptUpdateStatus status = DeliveryAttemptUpdateStatus::Invalid;
    DeliveryState state = DeliveryState::Failed;
    std::string clientMessageId;
    UserId sender = 0;
    UserId recipient = 0;
};

struct DeliverySnapshot
{
    DeliveryState state = DeliveryState::Failed;
    std::size_t attempts = 0;
    std::chrono::steady_clock::time_point nextActionAt{};
};

struct DeliveryRetry
{
    std::string serverMessageId;
    std::string clientMessageId;
    std::string content;
    UserId sender = 0;
    UserId recipient = 0;
    std::size_t attempt = 0;
};

struct DeliveryFailure
{
    std::string serverMessageId;
    std::string clientMessageId;
    UserId sender = 0;
    UserId recipient = 0;
};

struct DeliverySweep
{
    std::vector<DeliveryRetry> retries;
    std::vector<DeliveryFailure> failures;
    std::size_t transportTimeouts = 0;
    std::size_t ackTimeouts = 0;
    std::size_t retriesScheduled = 0;
};

struct WebSocketDeliveryConfig
{
    // 为空时 Tracker 生成进程实例 ID；测试或多实例部署可显式注入稳定的实例标识。
    std::string serverInstanceId;
    std::size_t maxRecords = 65536;
    std::size_t maxClientMessageIdBytes = 128;
    std::size_t maxServerMessageIdBytes = 128;
    std::size_t maxContentBytes = 1024 * 1024;
    std::size_t maxAttempts = 3;
    std::chrono::milliseconds transportTimeout{5000};
    std::chrono::milliseconds ackTimeout{5000};
    std::chrono::milliseconds retryDelay{500};       // attempt 2 前的初始退避
    std::chrono::milliseconds maxRetryDelay{30000};  // 指数退避上限
    std::uint32_t retryJitterPercent = 20;            // 确定性 ± 抖动，0 表示关闭
    std::chrono::milliseconds terminalRetention{60000};
};

class WebSocketDeliveryTracker
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit WebSocketDeliveryTracker(WebSocketDeliveryConfig config = {});

    DeliveryBeginResult begin(UserId sender,
                              UserId recipient,
                              std::string clientMessageId,
                              std::string content,
                              TimePoint now = Clock::now());

    DeliveryAckResult acknowledge(UserId recipient,
                                  const std::string &serverMessageId,
                                  TimePoint now = Clock::now());

    /**
     * @brief 回填某次发送的最终传输结果；attempt 防止旧回执覆盖较新的发送尝试
     */
    DeliveryAttemptUpdate recordAttemptResult(
        const std::string &serverMessageId,
        std::size_t attempt,
        DeliveryAttemptOutcome outcome,
        TimePoint now = Clock::now());

    bool markFailed(const std::string &serverMessageId,
                    TimePoint now = Clock::now());

    /**
     * @brief 推进超时状态，返回“应重发”和“已耗尽尝试”的动作，不执行任何 I/O
     */
    DeliverySweep collectDue(TimePoint now = Clock::now());

    std::size_t recordCount() const;
    std::size_t pendingCount() const;
    std::optional<DeliverySnapshot> snapshot(
        const std::string &serverMessageId) const;

private:
    struct ClientKey
    {
        UserId sender = 0;
        std::string clientMessageId;

        bool operator==(const ClientKey &other) const
        {
            return sender == other.sender &&
                   clientMessageId == other.clientMessageId;
        }
    };

    struct ClientKeyHash
    {
        std::size_t operator()(const ClientKey &key) const;
    };

    struct Record
    {
        std::string serverMessageId;
        std::string clientMessageId;
        std::string content;
        UserId sender = 0;
        UserId recipient = 0;
        DeliveryState state = DeliveryState::AwaitingTransport;
        std::size_t attempts = 1;
        TimePoint nextAttempt{};
        TimePoint removeAfter = TimePoint::max();
    };

    void pruneTerminals(TimePoint now);
    std::chrono::milliseconds retryDelayFor(
        const Record &record,
        std::size_t nextAttempt) const;
    std::string nextServerMessageId(UserId sender);
    static DeliveryAckResult ackResult(DeliveryAckStatus status,
                                       const Record *record);

    WebSocketDeliveryConfig config_;
    mutable std::mutex mtx_;
    std::uint64_t nextSequence_ = 1;
    std::unordered_map<std::string, Record> recordsByServerId_;
    std::unordered_map<ClientKey, std::string, ClientKeyHash> serverIdByClientKey_;
};

#endif
