#include "WebSocketDeliveryService.h"

#include "server/websocket/WebSocketCodec/WebSocketCodec.h"
#include "server/websocket/WebSocketMessage/WebSocketMessage.h"

#include <algorithm>
#include <utility>

namespace
{
bool isTerminalState(DeliveryState state)
{
    return state == DeliveryState::Acknowledged ||
           state == DeliveryState::Failed;
}

DeliveryAttemptOutcome mapOutcome(OutboundOutcome outcome)
{
    if (outcome == OutboundOutcome::Written)
        return DeliveryAttemptOutcome::Written;
    if (outcome == OutboundOutcome::Invalid)
        return DeliveryAttemptOutcome::PermanentFailure;
    return DeliveryAttemptOutcome::RetryableFailure;
}
}

WebSocketDeliveryService::WebSocketDeliveryService(
    SendFunction send,
    WebSocketDeliveryServiceConfig config)
    : send_(std::move(send)),
      pollInterval_(config.pollInterval <= std::chrono::milliseconds::zero()
                        ? std::chrono::milliseconds{1}
                        : config.pollInterval),
      tracker_(std::move(config.tracker))
{
}

WebSocketDeliveryService::~WebSocketDeliveryService()
{
    stop();
}

void WebSocketDeliveryService::start()
{
    std::lock_guard lock(lifecycleMutex_);
    if (worker_.joinable())
        return;
    std::unique_lock activityLock(activityMutex_);
    std::jthread worker(
        [this](std::stop_token stopToken)
        {
            workerLoop(stopToken);
        });
    worker_ = std::move(worker);
    accepting_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
}

void WebSocketDeliveryService::stop()
{
    std::unique_lock lifecycleLock(lifecycleMutex_);
    // 与 start 串行化；锁持有到 join/清理结束，禁止停旧线程时又启动一条新线程。
    accepting_.store(false, std::memory_order_release);
    std::jthread worker;
    if (worker_.joinable())
    {
        worker_.request_stop();
        wakeCv_.notify_all();
        worker = std::move(worker_);
    }
    if (worker.joinable())
        worker.join();
    std::unique_lock activityLock(activityMutex_);
    running_.store(false, std::memory_order_release);
    std::lock_guard lock(observationsMutex_);
    observations_.clear();
}

bool WebSocketDeliveryService::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

DeliverySubmissionResult WebSocketDeliveryService::submit(
    UserId sender,
    UserId recipient,
    std::string clientMessageId,
    std::string content,
    TimePoint now)
{
    submissions_.fetch_add(1, std::memory_order_relaxed);
    std::shared_lock activityLock(activityMutex_);
    if (!accepting_.load(std::memory_order_acquire))
    {
        rejectedSubmissions_.fetch_add(1, std::memory_order_relaxed);
        DeliverySubmissionResult unavailable;
        unavailable.status = DeliverySubmissionStatus::Unavailable;
        return unavailable;
    }
    const auto begun = tracker_.begin(
        sender, recipient, clientMessageId, content, now);
    DeliverySubmissionResult result;
    result.serverMessageId = begun.serverMessageId;
    result.state = begun.state;

    switch (begun.status)
    {
    case DeliveryBeginStatus::Created:
    {
        newMessages_.fetch_add(1, std::memory_order_relaxed);
        DeliveryRetry first;
        first.serverMessageId = begun.serverMessageId;
        first.clientMessageId = std::move(clientMessageId);
        first.content = std::move(content);
        first.sender = sender;
        first.recipient = recipient;
        first.attempt = 1;
        const auto dispatched = dispatchAttempt(first, now);
        result.admission = dispatched.admission;
        result.state = dispatched.state;
        if (dispatched.state == DeliveryState::Failed)
            result.status = DeliverySubmissionStatus::Failed;
        else if (dispatched.admission == EnqueueResult::Ok)
            result.status = DeliverySubmissionStatus::Accepted;
        else
            result.status = DeliverySubmissionStatus::RetryScheduled;
        break;
    }
    case DeliveryBeginStatus::Duplicate:
        duplicateSubmissions_.fetch_add(1, std::memory_order_relaxed);
        result.status = begun.state == DeliveryState::Acknowledged
                            ? DeliverySubmissionStatus::Acknowledged
                        : begun.state == DeliveryState::Failed
                            ? DeliverySubmissionStatus::Failed
                            : DeliverySubmissionStatus::DuplicatePending;
        break;
    case DeliveryBeginStatus::Conflict:
        rejectedSubmissions_.fetch_add(1, std::memory_order_relaxed);
        result.status = DeliverySubmissionStatus::Conflict;
        break;
    case DeliveryBeginStatus::Capacity:
        rejectedSubmissions_.fetch_add(1, std::memory_order_relaxed);
        result.status = DeliverySubmissionStatus::Capacity;
        break;
    case DeliveryBeginStatus::Invalid:
        rejectedSubmissions_.fetch_add(1, std::memory_order_relaxed);
        result.status = DeliverySubmissionStatus::Invalid;
        break;
    }
    return result;
}

DeliveryAckResult WebSocketDeliveryService::acknowledge(
    UserId recipient,
    const std::string &serverMessageId,
    TimePoint now)
{
    ackRequests_.fetch_add(1, std::memory_order_relaxed);
    std::shared_lock activityLock(activityMutex_);
    if (!accepting_.load(std::memory_order_acquire))
    {
        rejectedAcks_.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const auto result = tracker_.acknowledge(recipient, serverMessageId, now);
    if (result.status == DeliveryAckStatus::Acknowledged)
    {
        acknowledgedMessages_.fetch_add(1, std::memory_order_relaxed);
        removeObservations(serverMessageId);
        notifySender(result);
    }
    else if (result.status == DeliveryAckStatus::Duplicate)
        duplicateAcks_.fetch_add(1, std::memory_order_relaxed);
    else
        rejectedAcks_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

DeliverySweep WebSocketDeliveryService::tick(TimePoint now)
{
    std::shared_lock activityLock(activityMutex_);
    if (!accepting_.load(std::memory_order_acquire))
        return {};
    std::lock_guard tickLock(tickMutex_);
    harvestReceipts(now);
    auto sweep = tracker_.collectDue(now);
    transportTimeouts_.fetch_add(
        sweep.transportTimeouts, std::memory_order_relaxed);
    ackTimeouts_.fetch_add(sweep.ackTimeouts, std::memory_order_relaxed);
    retrySchedules_.fetch_add(
        sweep.retriesScheduled, std::memory_order_relaxed);
    failedMessages_.fetch_add(
        sweep.failures.size(), std::memory_order_relaxed);

    for (const auto &failure : sweep.failures)
    {
        removeObservations(failure.serverMessageId);
        notifySender(failure);
    }

    for (const auto &retry : sweep.retries)
    {
        const auto dispatched = dispatchAttempt(retry, now);
        if (dispatched.failedNow)
        {
            DeliveryFailure failure{
                retry.serverMessageId,
                retry.clientMessageId,
                retry.sender,
                retry.recipient};
            sweep.failures.push_back(failure);
            removeObservations(retry.serverMessageId);
            notifySender(failure);
        }
    }
    return sweep;
}

std::size_t WebSocketDeliveryService::pendingCount() const
{
    return tracker_.pendingCount();
}

std::size_t WebSocketDeliveryService::observedReceiptCount() const
{
    std::lock_guard lock(observationsMutex_);
    return observations_.size();
}

WebSocketDeliveryMetricsSnapshot WebSocketDeliveryService::metrics() const
{
    WebSocketDeliveryMetricsSnapshot result;
    result.submissions = submissions_.load(std::memory_order_relaxed);
    result.newMessages = newMessages_.load(std::memory_order_relaxed);
    result.duplicateSubmissions =
        duplicateSubmissions_.load(std::memory_order_relaxed);
    result.rejectedSubmissions =
        rejectedSubmissions_.load(std::memory_order_relaxed);
    result.attemptsDispatched =
        attemptsDispatched_.load(std::memory_order_relaxed);
    result.retriesDispatched =
        retriesDispatched_.load(std::memory_order_relaxed);
    result.writtenAttempts = writtenAttempts_.load(std::memory_order_relaxed);
    result.retryableAttemptFailures =
        retryableAttemptFailures_.load(std::memory_order_relaxed);
    result.permanentAttemptFailures =
        permanentAttemptFailures_.load(std::memory_order_relaxed);
    result.retrySchedules = retrySchedules_.load(std::memory_order_relaxed);
    result.transportTimeouts = transportTimeouts_.load(std::memory_order_relaxed);
    result.ackTimeouts = ackTimeouts_.load(std::memory_order_relaxed);
    result.ackRequests = ackRequests_.load(std::memory_order_relaxed);
    result.acknowledgedMessages =
        acknowledgedMessages_.load(std::memory_order_relaxed);
    result.duplicateAcks = duplicateAcks_.load(std::memory_order_relaxed);
    result.rejectedAcks = rejectedAcks_.load(std::memory_order_relaxed);
    result.failedMessages = failedMessages_.load(std::memory_order_relaxed);
    result.ignoredAttemptResults =
        ignoredAttemptResults_.load(std::memory_order_relaxed);
    result.pendingMessages = pendingCount();
    result.observedReceipts = observedReceiptCount();
    return result;
}

WebSocketDeliveryService::AttemptDispatch
WebSocketDeliveryService::dispatchAttempt(
    const DeliveryRetry &retry,
    TimePoint now)
{
    const auto beforeSend = tracker_.snapshot(retry.serverMessageId);
    if (!beforeSend ||
        beforeSend->state != DeliveryState::AwaitingTransport ||
        beforeSend->attempts != retry.attempt)
    {
        return {
            EnqueueResult::Invalid,
            beforeSend ? beforeSend->state : DeliveryState::Failed,
            false};
    }

    attemptsDispatched_.fetch_add(1, std::memory_order_relaxed);
    if (retry.attempt > 1)
        retriesDispatched_.fetch_add(1, std::memory_order_relaxed);

    WebSocketMessage message;
    message.type = "chat";
    message.messageId = retry.serverMessageId;
    message.fromUserId = retry.sender;
    message.toUserId = retry.recipient;
    message.text = retry.content;
    message.ackRequested = true;
    message.attempt = retry.attempt;

    OutboundSubmission submission;
    try
    {
        if (!send_)
            throw std::bad_function_call{};
        submission = send_(
            retry.recipient,
            WebSocketCodec::serializeApplicationMessage(message));
    }
    catch (...)
    {
        const auto update = tracker_.recordAttemptResult(
            retry.serverMessageId,
            retry.attempt,
            DeliveryAttemptOutcome::RetryableFailure,
            now);
        recordAttemptOutcome(DeliveryAttemptOutcome::RetryableFailure, update);
        wakeWorker();
        return {
            EnqueueResult::Closed,
            update.state,
            update.status == DeliveryAttemptUpdateStatus::Applied &&
                update.state == DeliveryState::Failed};
    }

    OutboundOutcome outcome = OutboundOutcome::Pending;
    if (submission.receipt)
        outcome = submission.receipt->outcome();

    if (submission.admission == EnqueueResult::Ok &&
        submission.receipt && outcome == OutboundOutcome::Pending)
    {
        {
            std::lock_guard lock(observationsMutex_);
            observations_.push_back({
                retry.serverMessageId, retry.attempt, submission.receipt});
        }
        // ACK 可能在 receipt 入表前已经到达；二次检查终态，防止留下孤儿观察项。
        if (const auto snapshot = tracker_.snapshot(retry.serverMessageId);
            !snapshot || isTerminalState(snapshot->state))
            removeObservations(retry.serverMessageId);
        wakeWorker();
        return {
            submission.admission,
            DeliveryState::AwaitingTransport,
            false};
    }

    DeliveryAttemptOutcome attemptOutcome;
    if (submission.admission == EnqueueResult::Invalid ||
        outcome == OutboundOutcome::Invalid)
        attemptOutcome = DeliveryAttemptOutcome::PermanentFailure;
    else if (outcome == OutboundOutcome::Written)
        attemptOutcome = DeliveryAttemptOutcome::Written;
    else
        attemptOutcome = DeliveryAttemptOutcome::RetryableFailure;

    const auto update = tracker_.recordAttemptResult(
        retry.serverMessageId, retry.attempt, attemptOutcome, now);
    recordAttemptOutcome(attemptOutcome, update);
    wakeWorker();
    return {
        submission.admission,
        update.state,
        update.status == DeliveryAttemptUpdateStatus::Applied &&
            update.state == DeliveryState::Failed};
}

void WebSocketDeliveryService::harvestReceipts(TimePoint now)
{
    std::vector<ReceiptObservation> completed;
    {
        std::lock_guard lock(observationsMutex_);
        auto it = observations_.begin();
        while (it != observations_.end())
        {
            if (it->receipt &&
                it->receipt->outcome() == OutboundOutcome::Pending)
            {
                ++it;
                continue;
            }
            completed.push_back(std::move(*it));
            it = observations_.erase(it);
        }
    }

    for (const auto &observation : completed)
    {
        const auto outcome = observation.receipt
                                 ? observation.receipt->outcome()
                                 : OutboundOutcome::Closed;
        const auto update = tracker_.recordAttemptResult(
            observation.serverMessageId,
            observation.attempt,
            mapOutcome(outcome),
            now);
        recordAttemptOutcome(mapOutcome(outcome), update);
        if (update.status == DeliveryAttemptUpdateStatus::Applied &&
            update.state == DeliveryState::Failed)
        {
            DeliveryFailure failure{
                observation.serverMessageId,
                update.clientMessageId,
                update.sender,
                update.recipient};
            removeObservations(observation.serverMessageId);
            notifySender(failure);
        }
    }
}

void WebSocketDeliveryService::recordAttemptOutcome(
    DeliveryAttemptOutcome outcome,
    const DeliveryAttemptUpdate &update) noexcept
{
    switch (outcome)
    {
    case DeliveryAttemptOutcome::Written:
        writtenAttempts_.fetch_add(1, std::memory_order_relaxed);
        break;
    case DeliveryAttemptOutcome::RetryableFailure:
        retryableAttemptFailures_.fetch_add(1, std::memory_order_relaxed);
        break;
    case DeliveryAttemptOutcome::PermanentFailure:
        permanentAttemptFailures_.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    if (update.status != DeliveryAttemptUpdateStatus::Applied)
    {
        ignoredAttemptResults_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (update.state == DeliveryState::RetryScheduled)
        retrySchedules_.fetch_add(1, std::memory_order_relaxed);
    else if (update.state == DeliveryState::Failed)
        failedMessages_.fetch_add(1, std::memory_order_relaxed);
}

void WebSocketDeliveryService::removeObservations(
    const std::string &serverMessageId)
{
    std::lock_guard lock(observationsMutex_);
    std::erase_if(
        observations_,
        [&](const ReceiptObservation &observation)
        {
            return observation.serverMessageId == serverMessageId;
        });
}

void WebSocketDeliveryService::notifySender(const DeliveryAckResult &ack)
{
    WebSocketMessage notice;
    notice.type = "delivery";
    notice.messageId = ack.serverMessageId;
    notice.replyTo = ack.clientMessageId;
    notice.fromUserId = ack.recipient;
    notice.toUserId = ack.sender;
    notice.status = "acknowledged";
    try
    {
        if (send_)
            (void)send_(
                ack.sender,
                WebSocketCodec::serializeApplicationMessage(notice));
    }
    catch (...)
    {
    }
}

void WebSocketDeliveryService::notifySender(const DeliveryFailure &failure)
{
    WebSocketMessage notice;
    notice.type = "delivery";
    notice.messageId = failure.serverMessageId;
    notice.replyTo = failure.clientMessageId;
    notice.toUserId = failure.sender;
    notice.status = "failed";
    try
    {
        if (send_)
            (void)send_(
                failure.sender,
                WebSocketCodec::serializeApplicationMessage(notice));
    }
    catch (...)
    {
    }
}

void WebSocketDeliveryService::wakeWorker()
{
    {
        std::lock_guard lock(wakeMutex_);
        wakeRequested_ = true;
    }
    wakeCv_.notify_all();
}

void WebSocketDeliveryService::workerLoop(std::stop_token stopToken)
{
    std::unique_lock lock(wakeMutex_);
    while (!stopToken.stop_requested())
    {
        wakeCv_.wait_for(
            lock,
            stopToken,
            pollInterval_,
            [this]
            {
                return wakeRequested_;
            });
        wakeRequested_ = false;
        if (stopToken.stop_requested())
            break;
        lock.unlock();
        try
        {
            (void)tick();
        }
        catch (...)
        {
            // 单次调度异常不能让后台线程静默消失；下一个 poll 周期继续推进。
        }
        lock.lock();
    }
}
