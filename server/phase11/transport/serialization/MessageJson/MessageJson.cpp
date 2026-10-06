#include "server/phase11/transport/serialization/MessageJson/MessageJson.h"

#include "server/phase11/transport/FlatJson/FlatJson.h"

#include <chrono>

namespace webserver::phase11::transport
{
namespace
{
std::int64_t epochMilliseconds(TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               value.time_since_epoch()).count();
}

std::string attachmentJson(const MessageAttachment &attachment)
{
    return "{\"fileId\":" + std::to_string(attachment.fileId) +
           ",\"name\":" + quoteJson(attachment.originalName) +
           ",\"mediaType\":" + quoteJson(attachment.mediaType) +
           ",\"size\":" + std::to_string(attachment.size) +
           ",\"sha256\":" + quoteJson(attachment.sha256) +
           ",\"url\":" + quoteJson("/api/files/" +
                                      std::to_string(attachment.fileId)) + "}";
}
} // namespace

std::string serializeMessageJson(const Message &message,
                                 std::string_view eventType)
{
    const bool recalled = message.state == MessageState::Recalled;
    std::string json{"{"};
    if (!eventType.empty()) json += "\"type\":" + quoteJson(eventType) + ",";
    json += "\"id\":" + std::to_string(message.id) +
            ",\"conversationId\":" + std::to_string(message.conversationId) +
            ",\"sequence\":" + std::to_string(message.sequence) +
            ",\"senderId\":" + std::to_string(message.senderId) +
            ",\"clientMessageId\":" + quoteJson(message.clientMessageId) +
            ",\"body\":" + quoteJson(recalled ? "" : message.body) +
            ",\"createdAt\":" + std::to_string(epochMilliseconds(message.createdAt)) +
            ",\"state\":" + quoteJson(recalled ? "recalled" : "active") +
            ",\"recalledBy\":" +
            (message.recalledBy ? std::to_string(*message.recalledBy) : "null") +
            ",\"recalledAt\":" +
            (message.recalledAt ? std::to_string(epochMilliseconds(*message.recalledAt))
                                : "null") +
            ",\"modelNodeId\":" +
            (message.modelTrace.nodeId ? std::to_string(*message.modelTrace.nodeId)
                                       : "null") +
            ",\"modelVersionId\":" +
            (message.modelTrace.versionId
                 ? std::to_string(*message.modelTrace.versionId) : "null") +
            ",\"adapterName\":" + quoteJson(message.modelTrace.adapterName) +
            ",\"attachments\":[";
    if (!recalled)
    {
        bool first = true;
        for (const auto &attachment : message.attachments)
        {
            if (!first) json.push_back(',');
            first = false;
            json += attachmentJson(attachment);
        }
    }
    json += "]}";
    return json;
}

} // namespace webserver::phase11::transport
