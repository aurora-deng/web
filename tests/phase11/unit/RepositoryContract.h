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
    contractRequire(repositories.social.updateContactSettings(
                        alice.id, bob.id, "contract friend", true),
                    "contact settings update failed");
    const auto contact = repositories.social.contactSettings(alice.id, bob.id);
    contractRequire(contact.remark == "contract friend" &&
                        contact.notificationsMuted,
                    "contact settings were not persisted");
    contractRequire(repositories.social.updateLastActive(bob.id, now),
                    "last-active update failed");
    contractRequire(repositories.social.lastActive(bob.id).has_value(),
                    "last-active lookup failed");

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
    contractRequire(repositories.social.blockUser(alice.id, moderator.id, now),
                    "block relationship was not persisted");
    contractRequire(repositories.social.isBlocked(alice.id, moderator.id) &&
                        repositories.social.blockedEitherDirection(
                            moderator.id, alice.id),
                    "block lookup failed");
    const auto blocked = repositories.social.listBlockedIds(alice.id);
    contractRequire(blocked.size() == 1 && blocked.front() == moderator.id,
                    "blocked-user listing failed");
    contractRequire(repositories.social.unblockUser(alice.id, moderator.id),
                    "unblock failed");
    contractRequire(repositories.social.removeFriendship(alice.id, bob.id),
                    "friend removal failed");
    contractRequire(!repositories.social.areFriends(alice.id, bob.id),
                    "removed friendship remained in the repository");

    // 独立会话验证游标分页，避免修改上面状态机测试的消息数量假设。
    const auto pages = repositories.conversations.createConversation(
        {0, ConversationKind::Group, "contract-pages", alice.id, 1, now},
        {{0, alice.id, MemberRole::Owner, 0, 0, now},
         {0, bob.id, MemberRole::Member, 0, 0, now}});
    for (int index = 1; index <= 231; ++index)
        repositories.messages.appendMessage(
            {0, pages.id, 0, index == 2 ? bob.id : alice.id,
             index <= 2 ? "same-client-key" : "page-" + std::to_string(index),
             "page content", {}, now});
    const auto newest = repositories.messages.messagesBefore(pages.id, 0, 100);
    const auto older = repositories.messages.messagesBefore(pages.id, newest.front().sequence, 100);
    const auto earliest = repositories.messages.messagesBefore(pages.id, older.front().sequence, 100);
    contractRequire(newest.size() == 100 && newest.front().sequence == 132 &&
                    newest.back().sequence == 231 && older.size() == 100 &&
                    older.front().sequence == 32 && older.back().sequence == 131 &&
                    earliest.size() == 31 && earliest.front().sequence == 1 &&
                    earliest.back().sequence == 31,
                    "backward pages overlap, skip messages, or are not ascending");
    contractRequire(repositories.messages.messagesBefore(pages.id, 1, 100).empty(),
                    "before cursor is not exclusive");
    const auto ownAlice = repositories.messages.findOwnMessage(pages.id, alice.id, "same-client-key");
    const auto ownBob = repositories.messages.findOwnMessage(pages.id, bob.id, "same-client-key");
    contractRequire(ownAlice && ownBob && ownAlice->id != ownBob->id &&
                    !repositories.messages.findOwnMessage(pages.id, bob.id, "page-3") &&
                    !repositories.messages.findOwnMessage(conversation.id, alice.id, "same-client-key"),
                    "own-message lookup leaked across sender or conversation");
    // 这个辅助群在测试完后软解散，保持后续驱动扩展契约的活跃会话前置条件。
    contractRequire(repositories.conversations.dissolveConversation(pages.id, alice.id, now),
                    "pagination fixture cleanup failed");
}

} // namespace webserver::phase11::test
