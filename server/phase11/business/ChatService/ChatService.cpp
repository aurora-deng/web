#include "server/phase11/business/ChatService/ChatService.h"

#include <algorithm>
#include <set>

namespace webserver::phase11
{

Conversation ChatService::requireConversation(ConversationId conversation) const
{
    const auto found = conversations_.findConversation(conversation);
    if (!found)
        throw ApplicationError(ErrorCode::NotFound, "conversation not found");
    return *found;
}

ConversationMember ChatService::requireMember(
    UserId user, ConversationId conversation) const
{
    const auto member = conversations_.findMember(conversation, user);
    if (!member)
        throw ApplicationError(ErrorCode::Forbidden,
                               "user is not a conversation member");
    return *member;
}

Conversation ChatService::createDirectConversation(UserId requester,
                                                    UserId peer,
                                                    TimePoint now)
{
    if (requester == peer)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "direct conversation needs two users");
    const auto first = users_.findUser(requester);
    const auto second = users_.findUser(peer);
    if (!first || !second)
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    // AI 账号像系统客服：用户可以直接发起私聊；普通用户之间必须先成为好友。
    if (!first->aiAccount && !second->aiAccount &&
        !social_.areFriends(requester, peer))
        throw ApplicationError(ErrorCode::Forbidden,
                               "direct messages require friendship");
    if (const auto existing =
            conversations_.findDirectConversation(requester, peer))
        return *existing;

    auto transaction = database_.beginTransaction();
    Conversation conversation;
    conversation.kind = ConversationKind::Direct;
    conversation.createdBy = requester;
    conversation.createdAt = now;
    const auto created = conversations_.createConversation(
        std::move(conversation),
        {{0, requester, MemberRole::Member, 0, 0, now},
         {0, peer, MemberRole::Member, 0, 0, now}});
    outbox_.appendOutbox({0, "conversation.created", "conversation",
                          created.id, std::to_string(created.id), now, {}});
    transaction->commit();
    return created;
}

Conversation ChatService::createGroupConversation(
    UserId owner, std::string title, std::vector<UserId> members,
    TimePoint now)
{
    if (!users_.findUser(owner))
        throw ApplicationError(ErrorCode::NotFound, "owner not found");
    if (title.empty() || title.size() > 80)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "group title length must be between 1 and 80");

    std::set<UserId> uniqueMembers(members.begin(), members.end());
    uniqueMembers.insert(owner);
    if (uniqueMembers.size() > 500)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "group member limit exceeded");

    std::vector<ConversationMember> records;
    records.reserve(uniqueMembers.size());
    for (const auto user : uniqueMembers)
    {
        if (!users_.findUser(user))
            throw ApplicationError(ErrorCode::NotFound,
                                   "group member not found");
        records.push_back(
            {0, user, user == owner ? MemberRole::Owner : MemberRole::Member,
             0, 0, now});
    }

    auto transaction = database_.beginTransaction();
    Conversation conversation;
    conversation.kind = ConversationKind::Group;
    conversation.title = std::move(title);
    conversation.createdBy = owner;
    conversation.createdAt = now;
    const auto created = conversations_.createConversation(
        std::move(conversation), std::move(records));
    outbox_.appendOutbox({0, "conversation.created", "conversation",
                          created.id, std::to_string(created.id), now, {}});
    transaction->commit();
    return created;
}

void ChatService::addGroupMember(UserId actor, ConversationId conversation,
                                 UserId member, TimePoint now)
{
    const auto existing = requireConversation(conversation);
    const auto actorMembership = requireMember(actor, conversation);
    if (existing.kind != ConversationKind::Group ||
        actorMembership.role != MemberRole::Owner)
        throw ApplicationError(ErrorCode::Forbidden,
                               "only the group owner can add members");
    if (!users_.findUser(member))
        throw ApplicationError(ErrorCode::NotFound, "new member not found");

    auto transaction = database_.beginTransaction();
    if (!conversations_.addMember(
            {conversation, member, MemberRole::Member, 0, 0, now}))
        throw ApplicationError(ErrorCode::Conflict,
                               "user is already a group member");
    outbox_.appendOutbox({0, "conversation.member-added", "conversation",
                          conversation, std::to_string(member), now, {}});
    transaction->commit();
}

AppendMessageResult ChatService::sendText(
    UserId sender, ConversationId conversation,
    std::string clientMessageId, std::string body, TimePoint now,
    MessageModelTrace trace)
{
    requireMember(sender, conversation);
    if (clientMessageId.empty() || clientMessageId.size() > 128)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid client message id");
    if (body.empty() || body.size() > 16 * 1024)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "message body length is out of range");

    auto transaction = database_.beginTransaction();
    auto appended = messages_.appendMessage(
        {0, conversation, 0, sender, std::move(clientMessageId),
         body, std::move(trace), now});
    if (!appended.inserted)
    {
        // 相同幂等键只允许重放完全相同的命令。若正文不同，说明客户端错误复用了 ID。
        const auto &existing = appended.message;
        if (existing.body != body)
            throw ApplicationError(ErrorCode::Conflict,
                                   "client message id was reused with different content");
        transaction->commit();
        return appended;
    }
    outbox_.appendOutbox({0, "message.created", "message",
                          appended.message.id,
                          std::to_string(appended.message.id), now, {}});
    transaction->commit();
    return appended;
}

void ChatService::acknowledgeDelivery(UserId user,
                                      ConversationId conversation,
                                      std::uint64_t sequence)
{
    const auto current = requireConversation(conversation);
    requireMember(user, conversation);
    if (sequence >= current.nextSequence ||
        !conversations_.updateDelivered(conversation, user, sequence))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "delivery sequence is invalid");
}

void ChatService::markRead(UserId user, ConversationId conversation,
                           std::uint64_t sequence)
{
    const auto current = requireConversation(conversation);
    requireMember(user, conversation);
    if (sequence >= current.nextSequence ||
        !conversations_.updateRead(conversation, user, sequence))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "read sequence is invalid");
}

std::vector<Conversation> ChatService::conversations(UserId user) const
{
    if (!users_.findUser(user))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    return conversations_.listConversations(user);
}

std::vector<ConversationParticipant> ChatService::members(
    UserId user, ConversationId conversation) const
{
    requireMember(user, conversation);
    const auto memberships = conversations_.listMembers(conversation);
    std::vector<ConversationParticipant> participants;
    participants.reserve(memberships.size());
    for (const auto &membership : memberships)
    {
        const auto profile = users_.findUser(membership.userId);
        if (!profile)
            throw ApplicationError(ErrorCode::Unavailable,
                                   "conversation member profile is missing");
        participants.push_back({membership, *profile});
    }
    return participants;
}

std::vector<Message> ChatService::history(UserId user,
                                          ConversationId conversation,
                                          std::uint64_t afterSequence,
                                          std::size_t limit) const
{
    requireMember(user, conversation);
    if (limit == 0 || limit > 500)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "history limit must be between 1 and 500");
    return messages_.messagesAfter(conversation, afterSequence, limit);
}

std::vector<Message> ChatService::search(UserId user,
                                         ConversationId conversation,
                                         std::string_view query,
                                         std::size_t limit) const
{
    requireMember(user, conversation);
    if (query.empty() || query.size() > 256 || limit == 0 || limit > 100)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid search query or limit");
    return messages_.searchMessages(conversation, query, limit);
}

} // namespace webserver::phase11
