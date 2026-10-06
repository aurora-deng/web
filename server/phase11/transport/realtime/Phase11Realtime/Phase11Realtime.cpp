#include "server/phase11/transport/realtime/Phase11Realtime/Phase11Realtime.h"

#include "server/phase11/transport/FlatJson/FlatJson.h"
#include "server/sse/SseEvent.h"
#include "server/sse/SseSessionManager.h"
#include "server/websocket/WebSocketCodec/WebSocketCodec.h"
#include "server/websocket/WebSocketDispatcher/WebSocketDispatcher.h"
#include "server/websocket/WebSocketDispatcher/WsMessageContext.h"
#include "server/websocket/WebSocketSessionManager/WebSocketSessionManager.h"

#include <chrono>
#include <string>
#include <vector>

namespace webserver::phase11::transport
{
namespace
{

TimePoint now()
{
    return std::chrono::system_clock::now();
}

void setReply(WsMessageContext &context, std::string type,
              std::string status, const Message *message = nullptr)
{
    WebSocketMessage reply;
    reply.type = std::move(type);
    reply.status = std::move(status);
    reply.replyTo = context.inbound.messageId;
    reply.toUserId = context.uid;
    if (message)
    {
        reply.messageId = std::to_string(message->id);
        reply.conversationId = message->conversationId;
        reply.sequence = message->sequence;
    }
    context.outbound.type = reply.type;
    context.outbound.opcode = WsOpcode::Text;
    context.outbound.text = WebSocketCodec::serializeApplicationMessage(reply);
    context.hasOutbound = true;
}

template<class Operation>
bool handleRealtime(WsMessageContext &context, std::string_view errorType,
                    Operation &&operation)
{
    try
    {
        operation();
    }
    catch (const ApplicationError &failure)
    {
        setReply(context, std::string(errorType), failure.what());
    }
    catch (const std::exception &)
    {
        setReply(context, std::string(errorType), "internal server error");
    }
    return true;
}

std::string persistedMessageJson(const Message &message)
{
    const auto createdAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                               message.createdAt.time_since_epoch()).count();
    return "{\"type\":\"chat.message\",\"id\":" +
           quoteJson(std::to_string(message.id)) +
           ",\"conversationId\":" + std::to_string(message.conversationId) +
           ",\"sequence\":" + std::to_string(message.sequence) +
           ",\"senderId\":" + std::to_string(message.senderId) +
           ",\"clientMessageId\":" + quoteJson(message.clientMessageId) +
           ",\"body\":" + quoteJson(message.body) +
           ",\"createdAt\":" + std::to_string(createdAt) + "}";
}

} // namespace

void Phase11Realtime::registerHandlers(WebSocketDispatcher &dispatcher)
{
    dispatcher.on("chat.send", [this](WsMessageContext &context)
    {
        return handleRealtime(context, "chat.error", [&]
        {
            if (context.uid == 0 || context.inbound.conversationId == 0)
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "authenticated conversation is required");
            const auto result = runDatabase(
                [this, sender = static_cast<UserId>(context.uid),
                 conversation = context.inbound.conversationId,
                 id = context.inbound.messageId,
                 body = context.inbound.text]
                {
                    return chat_.sendText(sender, conversation, id, body, now());
                });
            setReply(context, "chat.accepted",
                     result.inserted ? "inserted" : "duplicate",
                     &result.message);
        });
    });

    dispatcher.on("chat.ack", [this](WsMessageContext &context)
    {
        return handleRealtime(context, "chat.error", [&]
        {
            runDatabase([this, user = static_cast<UserId>(context.uid),
                         conversation = context.inbound.conversationId,
                         sequence = context.inbound.sequence]
            {
                chat_.acknowledgeDelivery(user, conversation, sequence);
            });
            setReply(context, "chat.acknowledged", "accepted");
        });
    });

    dispatcher.on("chat.read", [this](WsMessageContext &context)
    {
        return handleRealtime(context, "chat.error", [&]
        {
            runDatabase([this, user = static_cast<UserId>(context.uid),
                         conversation = context.inbound.conversationId,
                         sequence = context.inbound.sequence]
            {
                chat_.markRead(user, conversation, sequence);
            });
            setReply(context, "chat.read", "accepted");
        });
    });

    if (!aiChat_)
        return;

    dispatcher.on("ai.generate", [this](WsMessageContext &context)
    {
        return handleRealtime(context, "ai.error", [&]
        {
            if (context.uid == 0 || context.inbound.conversationId == 0 ||
                context.inbound.toUserId == 0)
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "authenticated AI conversation is required");
            aiChat_->generate(
                {static_cast<UserId>(context.uid),
                 static_cast<UserId>(context.inbound.toUserId),
                 context.inbound.conversationId,
                 context.inbound.messageId,
                 context.inbound.text,
                 512});
            setReply(context, "ai.accepted", "accepted");
        });
    });

    dispatcher.on("ai.cancel", [this](WsMessageContext &context)
    {
        return handleRealtime(context, "ai.error", [&]
        {
            if (context.uid == 0 || context.inbound.replyTo.empty())
                throw ApplicationError(ErrorCode::InvalidArgument,
                                       "generation id is required");
            if (!aiChat_->cancel(static_cast<UserId>(context.uid),
                                 context.inbound.replyTo))
                throw ApplicationError(ErrorCode::NotFound,
                                       "active generation not found");
            setReply(context, "ai.cancel.accepted", "accepted");
        });
    });
}

void Phase11AiEventSink::token(const AiTokenEvent &event)
{
    SseEvent notification;
    notification.id = event.generationId + ":" +
                      std::to_string(event.tokenIndex);
    notification.eventName = "ai.token";
    notification.data = "{\"generationId\":" +
                        quoteJson(event.generationId) +
                        ",\"conversationId\":" +
                        std::to_string(event.conversationId) +
                        ",\"index\":" + std::to_string(event.tokenIndex) +
                        ",\"text\":" + quoteJson(event.text) + "}";
    for (const auto recipient : event.recipients)
        (void)sse_.publish(recipient, notification);
}

void Phase11AiEventSink::completed(const AiCompletedEvent &event)
{
    SseEvent notification;
    notification.id = event.generationId + ":completed";
    notification.eventName = "ai.completed";
    notification.data = "{\"generationId\":" +
                        quoteJson(event.generationId) +
                        ",\"conversationId\":" +
                        std::to_string(event.conversationId) +
                        ",\"success\":" +
                        (event.success ? "true" : "false") +
                        ",\"cancelled\":" +
                        (event.cancelled ? "true" : "false") +
                        ",\"error\":" + quoteJson(event.error);
    if (event.message)
    {
        notification.data += ",\"messageId\":" +
                             quoteJson(std::to_string(event.message->id)) +
                             ",\"sequence\":" +
                             std::to_string(event.message->sequence);
    }
    notification.data += "}";
    for (const auto recipient : event.recipients)
        (void)sse_.publish(recipient, notification);
}

bool Phase11RealtimeOutboxSink::publish(const OutboxEvent &event)
{
    if (event.type == "message.created")
    {
        const auto message = messages_.findMessage(event.aggregateId);
        if (!message)
            return false;
        const auto members = conversations_.listMembers(message->conversationId);
        std::vector<UserId> users;
        users.reserve(members.size());
        for (const auto &member : members)
            users.push_back(member.userId);

        const auto json = persistedMessageJson(*message);
        (void)webSockets_.broadcastText(users, json);
        for (const auto user : users)
        {
            SseEvent notification;
            notification.id = std::to_string(event.id);
            notification.eventName = "message.notification";
            notification.data = json;
            (void)sse_.publish(user, notification);
        }
        // 零在线连接不算失败：消息已经持久化，离线用户会从 history 按 sequence 恢复。
        return true;
    }

    if (event.type == "conversation.created")
    {
        const auto members = conversations_.listMembers(event.aggregateId);
        for (const auto &member : members)
        {
            SseEvent notification;
            notification.id = std::to_string(event.id);
            notification.eventName = "conversation.invited";
            notification.data = "{\"conversationId\":" +
                                std::to_string(event.aggregateId) + "}";
            (void)sse_.publish(member.userId, notification);
        }
        return true;
    }

    if (event.type == "conversation.member-added")
    {
        try
        {
            const auto user = static_cast<UserId>(std::stoull(event.payload));
            SseEvent notification;
            notification.id = std::to_string(event.id);
            notification.eventName = "conversation.invited";
            notification.data = "{\"conversationId\":" +
                                std::to_string(event.aggregateId) + "}";
            (void)sse_.publish(user, notification);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    if (event.type == "friend.request")
    {
        try
        {
            const auto receiver = static_cast<UserId>(std::stoull(event.payload));
            SseEvent notification;
            notification.id = std::to_string(event.id);
            notification.eventName = "friend.request";
            notification.data = "{\"requestId\":" +
                                std::to_string(event.aggregateId) + "}";
            (void)sse_.publish(receiver, notification);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // 当前版本尚无外部消费者的事件也要确认，否则一个已知但无需推送的事件会永久
    // 堵住后面的消息。事件的持久业务结果已经在同一事务中完成。
    return true;
}

Phase11OutboxPump::Phase11OutboxPump(IOutboxRepository &outbox,
                                     IOutboxEventSink &sink)
    : dispatcher_(outbox, sink), worker_([this] { run(); })
{
}

Phase11OutboxPump::~Phase11OutboxPump()
{
    stop();
}

void Phase11OutboxPump::stop() noexcept
{
    if (stopping_.exchange(true, std::memory_order_acq_rel))
        return;
    wake_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void Phase11OutboxPump::run() noexcept
{
    while (!stopping_.load(std::memory_order_acquire))
    {
        std::chrono::milliseconds delay{250};
        try
        {
            const auto result = dispatcher_.dispatchPending(100, now());
            if (result.published == 100)
                delay = std::chrono::milliseconds{1};
            else if (result.blocked)
                delay = std::chrono::milliseconds{500};
        }
        catch (...)
        {
            delay = std::chrono::seconds{1};
        }
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, delay, [this]
        {
            return stopping_.load(std::memory_order_acquire);
        });
    }
}

} // namespace webserver::phase11::transport
