#pragma once

#include "server/phase11/ports/Ports/Ports.h"

#include <stdexcept>
#include <string>

namespace webserver::phase11::test
{

struct RepositoryBundle
{
    IDatabase &database;
    IUserRepository &users;
    ISessionRepository &sessions;
    ISocialRepository &social;
    IConversationRepository &conversations;
    IMessageRepository &messages;
    IOutboxRepository &outbox;
};

inline void contractRequire(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(std::string{"repository contract: "} + message);
}

/**
 * 存储实现的统一“驾照考试”。内存版和将来的 PostgreSQL 版都调用本函数，因此数据库
 * 更换后仍必须保持事务、幂等键、会话游标和 Outbox 的相同行为。
 *
 * 调用前应提供一个隔离的空数据库/schema；名称使用固定前缀，方便测试结束后清理。
 */
inline void runRepositoryContract(RepositoryBundle repositories,
                                  TimePoint now)
{
    {
        auto transaction = repositories.database.beginTransaction();
        repositories.users.create(
            {0, "contract-rollback", "Rollback", {}, "hash",
             false, false, now});
        // 未 commit 的事务离开作用域必须自动回滚。
    }
    contractRequire(!repositories.users.findUserByName("contract-rollback"),
                    "uncommitted user survived rollback");

    auto alice = repositories.users.create(
        {0, "contract-alice", "Alice", {}, "hash-a", false, false, now});
    auto bob = repositories.users.create(
        {0, "contract-bob", "Bob", {}, "hash-b", false, false, now});

    const auto session = repositories.sessions.createSession(
        {"contract-session", alice.id, "contract-token-hash",
         "contract-csrf-hash", now, now + std::chrono::hours(1), {}});
    contractRequire(
        repositories.sessions.findSessionByTokenHash(session.tokenHash).has_value(),
        "session token lookup failed");

    const auto request = repositories.social.createFriendRequest(
        {0, alice.id, bob.id, FriendRequestState::Pending, now, {}});
    contractRequire(repositories.social.decideFriendRequest(
                        request.id, bob.id, FriendRequestState::Accepted, now),
                    "friend decision failed");
    contractRequire(repositories.social.areFriends(alice.id, bob.id),
                    "accepted friendship was not persisted");
    const auto aliceFriends = repositories.social.listFriendIds(alice.id);
    contractRequire(aliceFriends.size() == 1 && aliceFriends.front() == bob.id,
                    "friend listing did not return the accepted peer");

    auto transaction = repositories.database.beginTransaction();
    const auto conversation = repositories.conversations.createConversation(
        {0, ConversationKind::Direct, {}, alice.id, 1, now},
        {{0, alice.id, MemberRole::Member, 0, 0, now},
         {0, bob.id, MemberRole::Member, 0, 0, now}});
    const auto first = repositories.messages.appendMessage(
        {0, conversation.id, 0, alice.id, "contract-message", "hello",
         {}, now});
    const auto duplicate = repositories.messages.appendMessage(
        {0, conversation.id, 0, alice.id, "contract-message", "hello",
         {}, now});
    const auto event = repositories.outbox.appendOutbox(
        {0, "message.created", "message", first.message.id,
         std::to_string(first.message.id), now, {}});
    transaction->commit();

    const auto members = repositories.conversations.listMembers(conversation.id);
    contractRequire(members.size() == 2,
                    "conversation member listing returned the wrong size");

    contractRequire(first.inserted && !duplicate.inserted,
                    "message idempotency failed");
    contractRequire(first.message.id == duplicate.message.id &&
                        first.message.sequence == 1,
                    "duplicate message identity or sequence changed");
    contractRequire(repositories.messages.messagesAfter(
                        conversation.id, 0, 10).size() == 1,
                    "history contains an unexpected message count");
    const auto bobSummaries =
        repositories.conversations.listConversationSummaries(bob.id);
    contractRequire(bobSummaries.size() == 1 &&
                        bobSummaries.front().unreadCount == 1 &&
                        bobSummaries.front().lastReadSequence == 0,
                    "persistent unread summary is incorrect");
    contractRequire(repositories.outbox.pendingOutbox(10).size() == 1,
                    "outbox event was not persisted with the message");
    contractRequire(repositories.outbox.markPublished(event.id, now),
                    "outbox acknowledgement failed");
    contractRequire(repositories.outbox.pendingOutbox(10).empty(),
                    "acknowledged outbox event remained pending");

    const auto moderator = repositories.users.create(
        {0, "contract-moderator", "Moderator", {}, "hash-m",
         false, false, now});
    const auto group = repositories.conversations.createConversation(
        {0, ConversationKind::Group, "Contract Group", alice.id, 1, now},
        {{0, alice.id, MemberRole::Owner, 0, 0, now},
         {0, moderator.id, MemberRole::Member, 0, 0, now}});
    contractRequire(repositories.conversations.updateConversationProfile(
                        group.id, "Renamed Group", "Contract announcement"),
                    "group profile update failed");
    const auto updatedGroup =
        repositories.conversations.findConversation(group.id);
    contractRequire(updatedGroup && updatedGroup->title == "Renamed Group" &&
                        updatedGroup->announcement == "Contract announcement",
                    "group profile fields were not persisted");
    contractRequire(repositories.conversations.updateMemberRole(
                        group.id, moderator.id, MemberRole::Administrator),
                    "administrator role update failed");
    const auto mutedUntil = now + std::chrono::minutes(10);
    contractRequire(repositories.conversations.updateMemberMute(
                        group.id, moderator.id, mutedUntil),
                    "member mute update failed");
    const auto moderatorMembership =
        repositories.conversations.findMember(group.id, moderator.id);
    contractRequire(moderatorMembership &&
                        moderatorMembership->role == MemberRole::Administrator &&
                        moderatorMembership->mutedUntil &&
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            *moderatorMembership->mutedUntil - mutedUntil).count() == 0,
                    "member role or mute was not persisted");
    contractRequire(repositories.conversations.removeMember(
                        group.id, moderator.id),
                    "member removal failed");
    contractRequire(!repositories.conversations.findMember(
                        group.id, moderator.id),
                    "removed member remained in the repository");
    contractRequire(repositories.conversations.dissolveConversation(
                        group.id, alice.id, now),
                    "group dissolve failed");
    const auto dissolved = repositories.conversations.findConversation(group.id);
    contractRequire(dissolved &&
                        dissolved->state == ConversationState::Dissolved &&
                        dissolved->dissolvedBy == alice.id,
                    "dissolved group audit fields were not persisted");
    contractRequire(repositories.conversations.listConversations(alice.id).size() == 1,
                    "dissolved group remained in the active conversation list");
}

} // namespace webserver::phase11::test
