#include "WebSocketDeliveryTracker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <functional>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
bool isTerminal(DeliveryState state)
{
    return state == DeliveryState::Acknowledged ||
           state == DeliveryState::Failed;
}

bool validInstanceId(const std::string &value)
{
    if (value.empty() || value.size() > 48)
        return false;
    return std::all_of(value.begin(), value.end(), [](char current)
    {
        const auto byte = static_cast<unsigned char>(current);
        return std::isalnum(byte) != 0 || current == '_' || current == '-';
    });
}

std::string generateInstanceId()
{
    std::random_device random;
    const auto epoch = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const std::array<std::uint32_t, 2> entropy{
        random(),
        random()};

    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(16) << epoch
        << std::setw(8) << entropy[0]
        << std::setw(8) << entropy[1];
    return out.str();
}

std::uint64_t stableJitterHash(const std::string &messageId,
                               std::size_t attempt)
{
    // FNV-1a：不依赖共享随机数发生器，同一消息/attempt 的退避可以稳定复现。
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : messageId)
    {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    for (std::size_t index = 0; index < sizeof(attempt); ++index)
    {
        hash ^= static_cast<unsigned char>((attempt >> (index * 8U)) & 0xFFU);
        hash *= 1099511628211ULL;
    }
    return hash;
}
}

WebSocketDeliveryTracker::WebSocketDeliveryTracker(
    WebSocketDeliveryConfig config)
    : config_(std::move(config))
{
    if (config_.serverInstanceId.empty())
        config_.serverInstanceId = generateInstanceId();
    else if (!validInstanceId(config_.serverInstanceId))
        throw std::invalid_argument(
            "serverInstanceId must contain 1-48 letters, digits, '_' or '-'");

    config_.maxRecords = std::max<std::size_t>(1, config_.maxRecords);
    config_.maxAttempts = std::max<std::size_t>(1, config_.maxAttempts);
    // "ws-" + sender + instance + sequence；确保自定义上限能装入本实例生成的 ID。
    config_.maxServerMessageIdBytes =
        std::max(config_.serverInstanceId.size() + 48,
                 config_.maxServerMessageIdBytes);
    if (config_.transportTimeout <= std::chrono::milliseconds::zero())
        config_.transportTimeout = std::chrono::milliseconds{1};
    if (config_.ackTimeout <= std::chrono::milliseconds::zero())
        config_.ackTimeout = std::chrono::milliseconds{1};
    if (config_.retryDelay <= std::chrono::milliseconds::zero())
        config_.retryDelay = std::chrono::milliseconds{1};
    if (config_.maxRetryDelay < config_.retryDelay)
        config_.maxRetryDelay = config_.retryDelay;
    config_.retryJitterPercent =
        std::min<std::uint32_t>(100, config_.retryJitterPercent);
    if (config_.terminalRetention < std::chrono::milliseconds::zero())
        config_.terminalRetention = std::chrono::milliseconds::zero();
}

std::size_t WebSocketDeliveryTracker::ClientKeyHash::operator()(
    const ClientKey &key) const
{
    const auto first = std::hash<UserId>{}(key.sender);
    const auto second = std::hash<std::string>{}(key.clientMessageId);
    return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U));
}

DeliveryBeginResult WebSocketDeliveryTracker::begin(
    UserId sender,
    UserId recipient,
    std::string clientMessageId,
    std::string content,
    TimePoint now)
{
    if (sender == 0 || recipient == 0 || clientMessageId.empty() ||
        clientMessageId.size() > config_.maxClientMessageIdBytes ||
        content.size() > config_.maxContentBytes)
        return {};

    std::lock_guard lock(mtx_);
    pruneTerminals(now);

    ClientKey key{sender, clientMessageId};
    if (auto duplicate = serverIdByClientKey_.find(key);
        duplicate != serverIdByClientKey_.end())
    {
        const auto recordIt = recordsByServerId_.find(duplicate->second);
        if (recordIt == recordsByServerId_.end())
        {
            serverIdByClientKey_.erase(duplicate);
        }
        else
        {
            const auto &record = recordIt->second;
            const bool sameRequest =
                record.recipient == recipient && record.content == content;
            return {
                sameRequest ? DeliveryBeginStatus::Duplicate
                            : DeliveryBeginStatus::Conflict,
                record.serverMessageId,
                record.state};
        }
    }

    if (recordsByServerId_.size() >= config_.maxRecords)
        return {DeliveryBeginStatus::Capacity, {}, DeliveryState::Failed};

    Record record;
    record.serverMessageId = nextServerMessageId(sender);
    record.clientMessageId = std::move(clientMessageId);
    record.content = std::move(content);
    record.sender = sender;
    record.recipient = recipient;
    record.nextAttempt = now + config_.transportTimeout;

    const auto serverId = record.serverMessageId;
    serverIdByClientKey_.emplace(
        ClientKey{sender, record.clientMessageId}, serverId);
    recordsByServerId_.emplace(serverId, std::move(record));
    return {
        DeliveryBeginStatus::Created,
        serverId,
        DeliveryState::AwaitingTransport};
}

DeliveryAckResult WebSocketDeliveryTracker::acknowledge(
    UserId recipient,
    const std::string &serverMessageId,
    TimePoint now)
{
    if (recipient == 0 || serverMessageId.empty() ||
        serverMessageId.size() > config_.maxServerMessageIdBytes)
        return {};

    std::lock_guard lock(mtx_);
    pruneTerminals(now);
    const auto it = recordsByServerId_.find(serverMessageId);
    if (it == recordsByServerId_.end())
    {
        DeliveryAckResult result;
        result.status = DeliveryAckStatus::Unknown;
        result.serverMessageId = serverMessageId;
        return result;
    }

    auto &record = it->second;
    if (record.recipient != recipient)
        return ackResult(DeliveryAckStatus::WrongRecipient, &record);
    if (record.state == DeliveryState::Acknowledged)
        return ackResult(DeliveryAckStatus::Duplicate, &record);
    if (record.state == DeliveryState::Failed)
        return ackResult(DeliveryAckStatus::TooLate, &record);

    record.state = DeliveryState::Acknowledged;
    record.removeAfter = now + config_.terminalRetention;
    return ackResult(DeliveryAckStatus::Acknowledged, &record);
}

DeliveryAttemptUpdate WebSocketDeliveryTracker::recordAttemptResult(
    const std::string &serverMessageId,
    std::size_t attempt,
    DeliveryAttemptOutcome outcome,
    TimePoint now)
{
    if (serverMessageId.empty() || attempt == 0 ||
        serverMessageId.size() > config_.maxServerMessageIdBytes)
        return {};

    std::lock_guard lock(mtx_);
    pruneTerminals(now);
    const auto it = recordsByServerId_.find(serverMessageId);
    if (it == recordsByServerId_.end())
        return {
            DeliveryAttemptUpdateStatus::Unknown,
            DeliveryState::Failed,
            {},
            0,
            0};

    auto &record = it->second;
    if (isTerminal(record.state))
        return {
            DeliveryAttemptUpdateStatus::Terminal,
            record.state,
            record.clientMessageId,
            record.sender,
            record.recipient};
    if (record.state != DeliveryState::AwaitingTransport ||
        record.attempts != attempt)
        return {
            DeliveryAttemptUpdateStatus::StaleAttempt,
            record.state,
            record.clientMessageId,
            record.sender,
            record.recipient};

    switch (outcome)
    {
    case DeliveryAttemptOutcome::Written:
        record.state = DeliveryState::AwaitingAck;
        record.nextAttempt = now + config_.ackTimeout;
        break;
    case DeliveryAttemptOutcome::RetryableFailure:
        if (record.attempts >= config_.maxAttempts)
        {
            record.state = DeliveryState::Failed;
            record.removeAfter = now + config_.terminalRetention;
        }
        else
        {
            record.state = DeliveryState::RetryScheduled;
            record.nextAttempt = now +
                retryDelayFor(record, record.attempts + 1);
        }
        break;
    case DeliveryAttemptOutcome::PermanentFailure:
        record.state = DeliveryState::Failed;
        record.removeAfter = now + config_.terminalRetention;
        break;
    }
    return {
        DeliveryAttemptUpdateStatus::Applied,
        record.state,
        record.clientMessageId,
        record.sender,
        record.recipient};
}

bool WebSocketDeliveryTracker::markFailed(
    const std::string &serverMessageId,
    TimePoint now)
{
    std::lock_guard lock(mtx_);
    const auto it = recordsByServerId_.find(serverMessageId);
    if (it == recordsByServerId_.end() || isTerminal(it->second.state))
        return false;
    it->second.state = DeliveryState::Failed;
    it->second.removeAfter = now + config_.terminalRetention;
    return true;
}

DeliverySweep WebSocketDeliveryTracker::collectDue(TimePoint now)
{
    DeliverySweep sweep;
    std::lock_guard lock(mtx_);
    pruneTerminals(now);

    for (auto &[_, record] : recordsByServerId_)
    {
        if (isTerminal(record.state) || now < record.nextAttempt)
            continue;

        if (record.state != DeliveryState::RetryScheduled)
        {
            if (record.state == DeliveryState::AwaitingTransport)
                ++sweep.transportTimeouts;
            else if (record.state == DeliveryState::AwaitingAck)
                ++sweep.ackTimeouts;

            if (record.attempts >= config_.maxAttempts)
            {
                record.state = DeliveryState::Failed;
                record.removeAfter = now + config_.terminalRetention;
                sweep.failures.push_back({
                    record.serverMessageId,
                    record.clientMessageId,
                    record.sender,
                    record.recipient});
            }
            else
            {
                record.state = DeliveryState::RetryScheduled;
                record.nextAttempt = now +
                    retryDelayFor(record, record.attempts + 1);
                ++sweep.retriesScheduled;
            }
            continue;
        }

        if (record.attempts >= config_.maxAttempts)
        {
            record.state = DeliveryState::Failed;
            record.removeAfter = now + config_.terminalRetention;
            sweep.failures.push_back({
                record.serverMessageId,
                record.clientMessageId,
                record.sender,
                record.recipient});
            continue;
        }

        ++record.attempts;
        record.state = DeliveryState::AwaitingTransport;
        record.nextAttempt = now + config_.transportTimeout;
        sweep.retries.push_back({
            record.serverMessageId,
            record.clientMessageId,
            record.content,
            record.sender,
            record.recipient,
            record.attempts});
    }
    return sweep;
}

std::size_t WebSocketDeliveryTracker::recordCount() const
{
    std::lock_guard lock(mtx_);
    return recordsByServerId_.size();
}

std::size_t WebSocketDeliveryTracker::pendingCount() const
{
    std::lock_guard lock(mtx_);
    std::size_t count = 0;
    for (const auto &[_, record] : recordsByServerId_)
        if (!isTerminal(record.state))
            ++count;
    return count;
}

std::optional<DeliverySnapshot> WebSocketDeliveryTracker::snapshot(
    const std::string &serverMessageId) const
{
    std::lock_guard lock(mtx_);
    const auto it = recordsByServerId_.find(serverMessageId);
    if (it == recordsByServerId_.end())
        return std::nullopt;
    return DeliverySnapshot{
        it->second.state,
        it->second.attempts,
        it->second.nextAttempt};
}

void WebSocketDeliveryTracker::pruneTerminals(TimePoint now)
{
    for (auto it = recordsByServerId_.begin();
         it != recordsByServerId_.end();)
    {
        const auto &record = it->second;
        if (!isTerminal(record.state) || now < record.removeAfter)
        {
            ++it;
            continue;
        }

        serverIdByClientKey_.erase(
            ClientKey{record.sender, record.clientMessageId});
        it = recordsByServerId_.erase(it);
    }
}

std::chrono::milliseconds WebSocketDeliveryTracker::retryDelayFor(
    const Record &record,
    std::size_t nextAttempt) const
{
    auto base = config_.retryDelay;
    for (std::size_t current = 2;
         current < nextAttempt && base < config_.maxRetryDelay;
         ++current)
    {
        if (base.count() > config_.maxRetryDelay.count() / 2)
            base = config_.maxRetryDelay;
        else
            base *= 2;
    }
    base = std::min(base, config_.maxRetryDelay);

    if (config_.retryJitterPercent == 0)
        return base;

    const auto hash = stableJitterHash(record.serverMessageId, nextAttempt);
    const long double unit =
        static_cast<long double>(hash) /
        static_cast<long double>(std::numeric_limits<std::uint64_t>::max());
    const long double ratio =
        static_cast<long double>(config_.retryJitterPercent) / 100.0L;
    const long double factor = (1.0L - ratio) + (2.0L * ratio * unit);
    const long double candidate =
        static_cast<long double>(base.count()) * factor;
    const long double bounded = std::clamp(
        candidate,
        1.0L,
        static_cast<long double>(config_.maxRetryDelay.count()));
    return std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(bounded)};
}

std::string WebSocketDeliveryTracker::nextServerMessageId(UserId sender)
{
    std::string id;
    do
    {
        id = "ws-" + std::to_string(sender) + "-" +
             config_.serverInstanceId + "-" +
             std::to_string(nextSequence_++);
        if (nextSequence_ == 0)
            nextSequence_ = 1;
    } while (recordsByServerId_.find(id) != recordsByServerId_.end());
    return id;
}

DeliveryAckResult WebSocketDeliveryTracker::ackResult(
    DeliveryAckStatus status,
    const Record *record)
{
    if (!record)
    {
        DeliveryAckResult result;
        result.status = status;
        return result;
    }
    return {
        status,
        record->serverMessageId,
        record->clientMessageId,
        record->sender,
        record->recipient};
}
