// =============================================================================
// WebSocketDeliveryService：把纯投递状态机接到出站回执和可停止的周期线程
//
// 【邮局调度员】Tracker 只记账、不开车；Service 观察 OutboundReceipt，定期领取
// collectDue() 产生的重发单，并通过注入的 SendFunction 发出。析构会 stop+join，后台线程
// 不会越过宿主对象生命周期。
// =============================================================================
#pragma once
#ifndef WEBSOCKET_DELIVERY_SERVICE_H
#define WEBSOCKET_DELIVERY_SERVICE_H

#include "server/transport/OutboundTask.h"
#include "server/websocket/WebSocketDelivery/WebSocketDeliveryTracker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

enum class DeliverySubmissionStatus
{
    Accepted,
    RetryScheduled,
    DuplicatePending,
    Acknowledged,
    Failed,
    Conflict,
    Capacity,
    Unavailable,
    Invalid
};

struct DeliverySubmissionResult
{
    DeliverySubmissionStatus status = DeliverySubmissionStatus::Invalid;
    std::string serverMessageId;
    DeliveryState state = DeliveryState::Failed;
    EnqueueResult admission = EnqueueResult::Invalid;
};

struct WebSocketDeliveryServiceConfig
{
    WebSocketDeliveryConfig tracker;
    std::chrono::milliseconds pollInterval{25};
};

/**
 * @brief Service 生命周期内的累计计数与当前水位快照。
 *
 * 累计计数使用原子量，不要求跨字段强一致；它们用于运维趋势，不参与投递决策。
 */
struct WebSocketDeliveryMetricsSnapshot
{
    std::uint64_t submissions = 0;
    std::uint64_t newMessages = 0;
    std::uint64_t duplicateSubmissions = 0;
    std::uint64_t rejectedSubmissions = 0;
    std::uint64_t attemptsDispatched = 0;
    std::uint64_t retriesDispatched = 0;
    std::uint64_t writtenAttempts = 0;
    std::uint64_t retryableAttemptFailures = 0;
    std::uint64_t permanentAttemptFailures = 0;
    std::uint64_t retrySchedules = 0;
    std::uint64_t transportTimeouts = 0;
    std::uint64_t ackTimeouts = 0;
    std::uint64_t ackRequests = 0;
    std::uint64_t acknowledgedMessages = 0;
    std::uint64_t duplicateAcks = 0;
    std::uint64_t rejectedAcks = 0;
    std::uint64_t failedMessages = 0;
    std::uint64_t ignoredAttemptResults = 0;
    std::size_t pendingMessages = 0;
    std::size_t observedReceipts = 0;
};

class WebSocketDeliveryService
{
public:
    using Clock = WebSocketDeliveryTracker::Clock;
    using TimePoint = WebSocketDeliveryTracker::TimePoint;
    using SendFunction =
        std::function<OutboundSubmission(UserId, std::string)>;

    explicit WebSocketDeliveryService(
        SendFunction send,
        WebSocketDeliveryServiceConfig config = {});
    ~WebSocketDeliveryService();

    WebSocketDeliveryService(const WebSocketDeliveryService &) = delete;
    WebSocketDeliveryService &operator=(const WebSocketDeliveryService &) = delete;

    void start();
    void stop();
    bool running() const noexcept;

    DeliverySubmissionResult submit(
        UserId sender,
        UserId recipient,
        std::string clientMessageId,
        std::string content,
        TimePoint now = Clock::now());

    DeliveryAckResult acknowledge(
        UserId recipient,
        const std::string &serverMessageId,
        TimePoint now = Clock::now());

    /** 公开 tick 便于不用 sleep 的确定性单元测试；生产由内部 jthread 周期调用。 */
    DeliverySweep tick(TimePoint now = Clock::now());

    std::size_t pendingCount() const;
    std::size_t observedReceiptCount() const;
    WebSocketDeliveryMetricsSnapshot metrics() const;

private:
    struct ReceiptObservation
    {
        std::string serverMessageId;
        std::size_t attempt = 0;
        std::shared_ptr<OutboundReceipt> receipt;
    };

    struct AttemptDispatch
    {
        EnqueueResult admission = EnqueueResult::Invalid;
        DeliveryState state = DeliveryState::Failed;
        bool failedNow = false;
    };

    AttemptDispatch dispatchAttempt(const DeliveryRetry &retry, TimePoint now);
    void harvestReceipts(TimePoint now);
    void recordAttemptOutcome(
        DeliveryAttemptOutcome outcome,
        const DeliveryAttemptUpdate &update) noexcept;
    void removeObservations(const std::string &serverMessageId);
    void notifySender(const DeliveryAckResult &ack);
    void notifySender(const DeliveryFailure &failure);
    void wakeWorker();
    void workerLoop(std::stop_token stopToken);

    SendFunction send_;
    std::chrono::milliseconds pollInterval_;
    WebSocketDeliveryTracker tracker_;

    mutable std::mutex observationsMutex_;
    std::vector<ReceiptObservation> observations_;
    std::mutex tickMutex_;

    mutable std::mutex lifecycleMutex_;
    mutable std::shared_mutex activityMutex_;
    std::mutex wakeMutex_;
    std::condition_variable_any wakeCv_;
    bool wakeRequested_ = false;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> accepting_{true};

    std::atomic<std::uint64_t> submissions_{0};
    std::atomic<std::uint64_t> newMessages_{0};
    std::atomic<std::uint64_t> duplicateSubmissions_{0};
    std::atomic<std::uint64_t> rejectedSubmissions_{0};
    std::atomic<std::uint64_t> attemptsDispatched_{0};
    std::atomic<std::uint64_t> retriesDispatched_{0};
    std::atomic<std::uint64_t> writtenAttempts_{0};
    std::atomic<std::uint64_t> retryableAttemptFailures_{0};
    std::atomic<std::uint64_t> permanentAttemptFailures_{0};
    std::atomic<std::uint64_t> retrySchedules_{0};
    std::atomic<std::uint64_t> transportTimeouts_{0};
    std::atomic<std::uint64_t> ackTimeouts_{0};
    std::atomic<std::uint64_t> ackRequests_{0};
    std::atomic<std::uint64_t> acknowledgedMessages_{0};
    std::atomic<std::uint64_t> duplicateAcks_{0};
    std::atomic<std::uint64_t> rejectedAcks_{0};
    std::atomic<std::uint64_t> failedMessages_{0};
    std::atomic<std::uint64_t> ignoredAttemptResults_{0};
};

#endif
