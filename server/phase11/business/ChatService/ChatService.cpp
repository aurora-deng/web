#include "server/phase11/business/ChatService/ChatService.h"

#include <algorithm>
#include <chrono>
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

Conversation ChatService::requireActiveGroup(ConversationId conversation) const
{
    const auto found = requireConversation(conversation);
    if (found.kind != ConversationKind::Group)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "operation requires a group conversation");
    if (found.state != ConversationState::Active)
        throw ApplicationError(ErrorCode::Conflict,
                               "group conversation has been dissolved");
    return found;
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
    if (social_.blockedEitherDirection(requester, peer))
        throw ApplicationError(ErrorCode::Forbidden,
                               "direct messages are blocked by a privacy rule");
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
    auto transaction = database_.beginTransaction();
    (void)requireActiveGroup(conversation);
    const auto actorMembership = requireMember(actor, conversation);
    if (actorMembership.role != MemberRole::Owner &&
        actorMembership.role != MemberRole::Administrator)
        throw ApplicationError(ErrorCode::Forbidden,
                               "only group owners and administrators can add members");
    if (!users_.findUser(member))
        throw ApplicationError(ErrorCode::NotFound, "new member not found");
    if (conversations_.listMembers(conversation).size() >= 500)
        throw ApplicationError(ErrorCode::Conflict,
                               "group member limit exceeded");

    if (!conversations_.addMember(
            {conversation, member, MemberRole::Member, 0, 0, now}))
        throw ApplicationError(ErrorCode::Conflict,
                               "user is already a group member");
    outbox_.appendOutbox({0, "conversation.member-added", "conversation",
                          conversation, std::to_string(member), now, {}});
    transaction->commit();
}

Conversation ChatService::updateGroupProfile(
    UserId actor, ConversationId conversation, std::string title,
    std::string announcement, TimePoint now)
{
    auto transaction = database_.beginTransaction();
    (void)requireActiveGroup(conversation);
    const auto actorMembership = requireMember(actor, conversation);
    if (actorMembership.role == MemberRole::Member)
        throw ApplicationError(ErrorCode::Forbidden,
                               "only group owners and administrators can edit the group");
    if (title.empty() || title.size() > 80)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "group title length must be between 1 and 80");
    if (announcement.size() > 4096)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "group announcement is too long");

    if (!conversations_.updateConversationProfile(
            conversation, std::move(title), std::move(announcement)))
        throw ApplicationError(ErrorCode::Conflict,
                               "group profile could not be updated");
    outbox_.appendOutbox({0, "conversation.updated", "conversation",
                          conversation, {}, now, {}});
    transaction->commit();
    return requireConversation(conversation);
}

void ChatService::setGroupMemberRole(UserId actor,
                                     ConversationId conversation,
                                     UserId member, MemberRole role,
                                     TimePoint now)
{
    auto transaction = database_.beginTransaction();
    (void)requireActiveGroup(conversation);
    const auto actorMembership = requireMember(actor, conversation);
    const auto target = requireMember(member, conversation);
    if (actorMembership.role != MemberRole::Owner)
        throw ApplicationError(ErrorCode::Forbidden,
                               "only the group owner can change member roles");
    if (target.role == MemberRole::Owner || role == MemberRole::Owner)
        throw ApplicationError(ErrorCode::Forbidden,
                               "the owner role cannot be changed here");
    if (role != MemberRole::Member && role != MemberRole::Administrator)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "role must be member or administrator");
    if (target.role == role)
    {
        transaction->commit();
        return;
    }

    if (!conversations_.updateMemberRole(conversation, member, role))
        throw ApplicationError(ErrorCode::NotFound, "group member not found");
    outbox_.appendOutbox({0, "conversation.member-role-changed", "conversation",
                          conversation, std::to_string(member), now, {}});
    transaction->commit();
}

void ChatService::setGroupMemberMute(
    UserId actor, ConversationId conversation, UserId member,
    std::optional<TimePoint> mutedUntil, TimePoint now)
{
    auto transaction = database_.beginTransaction();
    (void)requireActiveGroup(conversation);
    const auto actorMembership = requireMember(actor, conversation);
    const auto target = requireMember(member, conversation);
    if (actorMembership.role == MemberRole::Member)
        throw ApplicationError(ErrorCode::Forbidden,
                               "only group owners and administrators can mute members");
    if (target.role == MemberRole::Owner ||
        (actorMembership.role == MemberRole::Administrator &&
         target.role != MemberRole::Member))
        throw ApplicationError(ErrorCode::Forbidden,
                               "the selected member cannot be muted by this actor");
    if (mutedUntil &&
        (*mutedUntil <= now || *mutedUntil > now + std::chrono::hours(24 * 30)))
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "mute duration must be between 1 second and 30 days");

    if (!conversations_.updateMemberMute(conversation, member, mutedUntil))
        throw ApplicationError(ErrorCode::NotFound, "group member not found");
    outbox_.appendOutbox({0, "conversation.member-muted", "conversation",
                          conversation, std::to_string(member), now, {}});
    transaction->commit();
}

void ChatService::removeGroupMember(UserId actor,
                                    ConversationId conversation,
                                    UserId member, TimePoint now)
{
    auto transaction = database_.beginTransaction();
    (void)requireActiveGroup(conversation);
    const auto actorMembership = requireMember(actor, conversation);
    const auto target = requireMember(member, conversation);
    if (actor == member)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "use leave group to remove yourself");
    if (actorMembership.role == MemberRole::Member)
        throw ApplicationError(ErrorCode::Forbidden,
                               "only group owners and administrators can remove members");
    if (target.role == MemberRole::Owner ||
        (actorMembership.role == MemberRole::Administrator &&
         target.role != MemberRole::Member))
        throw ApplicationError(ErrorCode::Forbidden,
                               "the selected member cannot be removed by this actor");

    if (!conversations_.removeMember(conversation, member))
        throw ApplicationError(ErrorCode::NotFound, "group member not found");
    outbox_.appendOutbox({0, "conversation.member-removed", "conversation",
                          conversation, std::to_string(member), now, {}});
    transaction->commit();
}

void ChatService::leaveGroup(UserId user, ConversationId conversation,
                             TimePoint now)
{
    auto transaction = database_.beginTransaction();
    (void)requireActiveGroup(conversation);
    const auto membership = requireMember(user, conversation);
    if (membership.role == MemberRole::Owner)
        throw ApplicationError(ErrorCode::Conflict,
                               "the group owner must dissolve the group");

    if (!conversations_.removeMember(conversation, user))
        throw ApplicationError(ErrorCode::NotFound, "group member not found");
    outbox_.appendOutbox({0, "conversation.member-left", "conversation",
                          conversation, std::to_string(user), now, {}});
    transaction->commit();
}

void ChatService::dissolveGroup(UserId actor, ConversationId conversation,
                                TimePoint now)
{
    auto transaction = database_.beginTransaction();
    (void)requireActiveGroup(conversation);
    const auto membership = requireMember(actor, conversation);
    if (membership.role != MemberRole::Owner)
        throw ApplicationError(ErrorCode::Forbidden,
                               "only the group owner can dissolve the group");

    if (!conversations_.dissolveConversation(conversation, actor, now))
        throw ApplicationError(ErrorCode::Conflict,
                               "group was already dissolved");
    outbox_.appendOutbox({0, "conversation.dissolved", "conversation",
                          conversation, std::to_string(actor), now, {}});
    transaction->commit();
}

AppendMessageResult ChatService::sendText(
    UserId sender, ConversationId conversation,
    std::string clientMessageId, std::string body, TimePoint now,
    MessageModelTrace trace)
{
    if (clientMessageId.empty() || clientMessageId.size() > 128)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid client message id");
    if (body.empty() || body.size() > 16 * 1024)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "message body length is out of range");

    auto transaction = database_.beginTransaction();
    const auto current = requireConversation(conversation);
    if (current.state != ConversationState::Active)
        throw ApplicationError(ErrorCode::Conflict,
                               "conversation has been dissolved");
    const auto membership = requireMember(sender, conversation);
    if (membership.mutedUntil && *membership.mutedUntil > now)
        throw ApplicationError(ErrorCode::Forbidden,
                               "group member is currently muted");
    if (current.kind == ConversationKind::Direct)
    {
        const auto members = conversations_.listMembers(conversation);
        const auto peer = std::find_if(
            members.begin(), members.end(), [sender](const auto &member)
            { return member.userId != sender; });
        if (peer == members.end() ||
            social_.blockedEitherDirection(sender, peer->userId))
            throw ApplicationError(ErrorCode::Forbidden,
                                   "direct messages are blocked by a privacy rule");
        const auto senderUser = users_.findUser(sender);
        const auto peerUser = users_.findUser(peer->userId);
        if (!senderUser || !peerUser)
            throw ApplicationError(ErrorCode::NotFound, "direct user not found");
        if (!senderUser->aiAccount && !peerUser->aiAccount &&
            !social_.areFriends(sender, peer->userId))
            throw ApplicationError(ErrorCode::Forbidden,
                                   "direct messages require friendship");
    }
    const auto originalBody = body;
    auto appended = messages_.appendMessage(
        {0, conversation, 0, sender, std::move(clientMessageId),
         body, std::move(trace), now});
    if (!appended.inserted)
    {
        // 相同幂等键只允许重放完全相同的命令。若正文不同，说明客户端错误复用了 ID。
        const auto &existing = appended.message;
        if (existing.body != originalBody)
            throw ApplicationError(ErrorCode::Conflict,
                                   "client message id was reused with different content");
        transaction->commit();
        return appended;
    }
    // 能发出消息，说明发送者已经进入并阅读到当前会话位置。同步推进自己的游标，
    // 避免刚发出的消息在刷新页面后反过来计入自己的未读数。
    (void)conversations_.updateRead(conversation, sender,
                                    appended.message.sequence);
    outbox_.appendOutbox({0, "message.created", "message",
                          appended.message.id,
                          std::to_string(appended.message.id), now, {}});
    transaction->commit();
    return appended;
}

RecallMessageResult ChatService::recallMessage(UserId actor,
                                               MessageId message,
                                               TimePoint now)
{
    auto transaction = database_.beginTransaction();
    const auto existing = messages_.findMessage(message);
    if (!existing)
        throw ApplicationError(ErrorCode::NotFound, "message not found");
    const auto conversation = requireConversation(existing->conversationId);
    if (conversation.state != ConversationState::Active)
        throw ApplicationError(ErrorCode::Conflict,
                               "conversation has been dissolved");
    const auto actorMembership = requireMember(actor, existing->conversationId);
    if (existing->senderId != actor)
    {
        if (conversation.kind != ConversationKind::Group ||
            actorMembership.role == MemberRole::Member)
            throw ApplicationError(ErrorCode::Forbidden,
                                   "only the sender or a group moderator can recall this message");
        const auto senderMembership = conversations_.findMember(
            existing->conversationId, existing->senderId);
        if (actorMembership.role == MemberRole::Administrator &&
            senderMembership && senderMembership->role != MemberRole::Member)
            throw ApplicationError(ErrorCode::Forbidden,
                                   "administrators cannot recall owner or administrator messages");
    }
    if (existing->state == MessageState::Recalled)
    {
        transaction->commit();
        return {*existing, false};
    }
    constexpr auto recallWindow = std::chrono::minutes(2);
    if (now < existing->createdAt || now - existing->createdAt > recallWindow)
        throw ApplicationError(ErrorCode::Conflict,
                               "message recall window has expired");

    auto recalled = messages_.recallMessage(message, actor, now);
    if (!recalled.message.id)
        throw ApplicationError(ErrorCode::NotFound, "message not found");
    if (recalled.changed)
        outbox_.appendOutbox({0, "message.recalled", "message", message,
                              std::to_string(existing->conversationId), now, {}});
    transaction->commit();
    return recalled;
}

void ChatService::acknowledgeDelivery(UserId user,
                                      ConversationId conversation,
                                      std::uint64_t sequence,
                                      TimePoint now)
{
    const auto current = requireConversation(conversation);
    const auto member = requireMember(user, conversation);
    if (sequence >= current.nextSequence)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "delivery sequence is invalid");
    if (sequence <= member.lastDeliveredSequence)
        return;
    auto transaction = database_.beginTransaction();
    if (conversations_.updateDelivered(conversation, user, sequence))
        outbox_.appendOutbox({0, "message.delivered", "conversation",
                              conversation,
                              std::to_string(user) + ":" +
                                  std::to_string(sequence), now, {}});
    transaction->commit();
}

void ChatService::markRead(UserId user, ConversationId conversation,
                           std::uint64_t sequence, TimePoint now)
{
    const auto current = requireConversation(conversation);
    const auto member = requireMember(user, conversation);
    if (sequence >= current.nextSequence)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "read sequence is invalid");
    if (sequence <= member.lastReadSequence)
        return;
    auto transaction = database_.beginTransaction();
    if (conversations_.updateRead(conversation, user, sequence))
        outbox_.appendOutbox({0, "message.read", "conversation", conversation,
                              std::to_string(user) + ":" +
                                  std::to_string(sequence), now, {}});
    transaction->commit();
}

std::vector<ConversationSummary> ChatService::conversations(UserId user) const
{
    if (!users_.findUser(user))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    return conversations_.listConversationSummaries(user);
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
