#include "WebSocketDeliveryTracker.h"

#include <algorithm>
#include <functional>
#include <utility>

WebSocketDeliveryTracker::WebSocketDeliveryTracker(
    WebSocketDeliveryConfig config)
    : config_(std::move(config))
{
    config_.maxRecords = std::max<std::size_t>(1, config_.maxRecords);
    config_.maxAttempts = std::max<std::size_t>(1, config_.maxAttempts);
    // "ws-" + 两个 uint64 十进制数 + 两个连字符最多 45 字节，留出余量。
    config_.maxServerMessageIdBytes =
        std::max<std::size_t>(64, config_.maxServerMessageIdBytes);
    if (config_.ackTimeout <= std::chrono::milliseconds::zero())
        config_.ackTimeout = std::chrono::milliseconds{1};
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
    record.nextAttempt = now + config_.ackTimeout;

    const auto serverId = record.serverMessageId;
    serverIdByClientKey_.emplace(
        ClientKey{sender, record.clientMessageId}, serverId);
    recordsByServerId_.emplace(serverId, std::move(record));
    return {
        DeliveryBeginStatus::Created,
        serverId,
        DeliveryState::AwaitingAck};
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

bool WebSocketDeliveryTracker::markFailed(
    const std::string &serverMessageId,
    TimePoint now)
{
    std::lock_guard lock(mtx_);
    const auto it = recordsByServerId_.find(serverMessageId);
    if (it == recordsByServerId_.end() ||
        it->second.state != DeliveryState::AwaitingAck)
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
        if (record.state != DeliveryState::AwaitingAck ||
            now < record.nextAttempt)
            continue;

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
        record.nextAttempt = now + config_.ackTimeout;
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
        if (record.state == DeliveryState::AwaitingAck)
            ++count;
    return count;
}

void WebSocketDeliveryTracker::pruneTerminals(TimePoint now)
{
    for (auto it = recordsByServerId_.begin();
         it != recordsByServerId_.end();)
    {
        const auto &record = it->second;
        if (record.state == DeliveryState::AwaitingAck ||
            now < record.removeAfter)
        {
            ++it;
            continue;
        }

        serverIdByClientKey_.erase(
            ClientKey{record.sender, record.clientMessageId});
        it = recordsByServerId_.erase(it);
    }
}

std::string WebSocketDeliveryTracker::nextServerMessageId(UserId sender)
{
    std::string id;
    do
    {
        id = "ws-" + std::to_string(sender) + "-" +
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
