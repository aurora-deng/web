#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/MessageTypes/MessageTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IMessageRepository
{
public:
    virtual ~IMessageRepository() = default;
    virtual AppendMessageResult appendMessage(Message message) = 0;
    [[nodiscard]] virtual std::vector<Message> messagesAfter(
        ConversationId conversation, std::uint64_t sequence,
        std::size_t limit) const = 0;
    /** before=0 取最新一页；否则取该序号之前的一页，结果始终按序号升序。 */
    [[nodiscard]] virtual std::vector<Message> messagesBefore(
        ConversationId conversation, std::uint64_t before,
        std::size_t limit) const = 0;
    /** 幂等键必须包含会话和发送者，不能通过客户端 ID 查询其他人的消息。 */
    [[nodiscard]] virtual std::optional<Message> findOwnMessage(
        ConversationId conversation, UserId sender,
        std::string_view clientMessageId) const = 0;
    [[nodiscard]] virtual std::vector<Message> searchMessages(
        ConversationId conversation, std::string_view query,
        std::size_t limit) const = 0;
    [[nodiscard]] virtual std::optional<Message> findMessage(MessageId id) const = 0;
    virtual RecallMessageResult recallMessage(MessageId id, UserId actor,
                                              TimePoint now) = 0;
};
}
