#include "backend/application/storage/memory/MemoryConversationRepository/MemoryConversationRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
Conversation MemoryConversationRepository::createConversation(storage_detail::MemoryState &memory, Conversation conversation, std::vector<ConversationMember> members)
{
    if (members.empty())
        throw std::invalid_argument("conversation requires members");
    conversation.id = memory.nextConversationId++;
    conversation.nextSequence = 1;
    memory.conversations.emplace(conversation.id, conversation);
    for (auto &member : members)
    {
        if (!memory.users.contains(member.userId))
            throw std::invalid_argument("conversation member does not exist");
        member.conversationId = conversation.id;
        memory.members.emplace(MemberKey{conversation.id, member.userId}, member);
    }
    return conversation;
}

std::optional<Conversation> MemoryConversationRepository::findConversation(const storage_detail::MemoryState &memory, ConversationId id)
{
    const auto found = memory.conversations.find(id);
    return found == memory.conversations.end()
               ? std::nullopt
               : std::optional<Conversation>{found->second};
}

std::optional<ConversationMember> MemoryConversationRepository::findMember(const storage_detail::MemoryState &memory, ConversationId conversation, UserId user)
{
    const auto found = memory.members.find({conversation, user});
    return found == memory.members.end()
               ? std::nullopt
               : std::optional<ConversationMember>{found->second};
}

std::optional<Conversation> MemoryConversationRepository::findDirectConversation(const storage_detail::MemoryState &memory, UserId first, UserId second)
{
    for (const auto &[id, conversation] : memory.conversations)
    {
        if (conversation.kind != ConversationKind::Direct ||
            conversation.state != ConversationState::Active)
            continue;
        if (memory.members.contains({id, first}) &&
            memory.members.contains({id, second}))
            return conversation;
    }
    return std::nullopt;
}

std::vector<Conversation> MemoryConversationRepository::listConversations(const storage_detail::MemoryState &memory, UserId user)
{
    std::vector<Conversation> result;
    for (const auto &[key, _] : memory.members)
        if (key.second == user)
        {
            const auto &conversation = memory.conversations.at(key.first);
            if (conversation.state == ConversationState::Active)
                result.push_back(conversation);
        }
    return result;
}

std::vector<ConversationSummary> MemoryConversationRepository::listConversationSummaries(const storage_detail::MemoryState &memory, UserId user)
{
    std::vector<ConversationSummary> result;
    for (const auto &[key, member] : memory.members)
    {
        if (key.second != user)
            continue;
        const auto &conversation = memory.conversations.at(key.first);
        if (conversation.state != ConversationState::Active)
            continue;
        const auto latest = conversation.nextSequence > 0
                                ? conversation.nextSequence - 1 : 0;
        result.push_back({conversation,
                          member.lastDeliveredSequence,
                          member.lastReadSequence,
                          latest > member.lastReadSequence
                              ? latest - member.lastReadSequence : 0});
        auto &summary = result.back();
        summary.displayTitle = conversation.title;
        for (const auto &[otherKey, otherMember] : memory.members) {
            if (otherKey.first != conversation.id) continue;
            const auto &other = memory.users.at(otherMember.userId);
            summary.containsAi = summary.containsAi || other.aiAccount;
            if (conversation.kind != ConversationKind::Direct || other.id == user) continue;
            summary.displayTitle = other.displayName;
            summary.avatarFileId = other.avatarFileId;
            summary.peerUserId=other.id;
            if (const auto preference = memory.contactSettings.find({user, other.id});
                preference != memory.contactSettings.end()) {
                if (!preference->second.remark.empty()) summary.displayTitle = preference->second.remark;
                summary.notificationsMuted = preference->second.notificationsMuted;
            }
        }
        if(auto preference=memory.conversationPreferences.find({conversation.id,user});preference!=memory.conversationPreferences.end()){
            summary.notificationsMuted=preference->second.muted;
            summary.preferencesRevision=preference->second.version;
        }
        if (const auto ids = memory.messageIds.find(conversation.id);
            ids != memory.messageIds.end() && !ids->second.empty()) {
            const auto &message = memory.messages.at(ids->second.back());
            summary.lastMessagePreview = message.state == MessageState::Recalled
                ? "这条消息已被撤回" : message.body.empty() ? "[附件]" : message.body;
            summary.lastMessageAt = message.createdAt;
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        const auto at = a.lastMessageAt == TimePoint{} ? a.conversation.createdAt : a.lastMessageAt;
        const auto bt = b.lastMessageAt == TimePoint{} ? b.conversation.createdAt : b.lastMessageAt;
        return at == bt ? a.conversation.id > b.conversation.id : at > bt;
    });
    return result;
}

std::vector<ConversationMember> MemoryConversationRepository::listMembers(const storage_detail::MemoryState &memory, ConversationId conversation)
{
    std::vector<ConversationMember> result;
    for (const auto &[key, member] : memory.members)
        if (key.first == conversation)
            result.push_back(member);
    return result;
}

bool MemoryConversationRepository::addMember(storage_detail::MemoryState &memory, ConversationMember member)
{
    const auto conversation = memory.conversations.find(member.conversationId);
    if (conversation == memory.conversations.end() ||
        conversation->second.state != ConversationState::Active ||
        !memory.users.contains(member.userId))
        return false;
    return memory.members.emplace(
        MemberKey{member.conversationId, member.userId}, member).second;
}

bool MemoryConversationRepository::updateConversationProfile(storage_detail::MemoryState &memory, ConversationId conversation, std::string title, std::string announcement)
{
    const auto found = memory.conversations.find(conversation);
    if (found == memory.conversations.end() ||
        found->second.kind != ConversationKind::Group ||
        found->second.state != ConversationState::Active)
        return false;
    found->second.title = std::move(title);
    found->second.announcement = std::move(announcement);
    return true;
}

bool MemoryConversationRepository::dissolveConversation(storage_detail::MemoryState &memory, ConversationId conversation, UserId actor, TimePoint now)
{
    const auto found = memory.conversations.find(conversation);
    if (found == memory.conversations.end() ||
        found->second.kind != ConversationKind::Group ||
        found->second.state != ConversationState::Active)
        return false;
    found->second.state = ConversationState::Dissolved;
    found->second.dissolvedBy = actor;
    found->second.dissolvedAt = now;
    return true;
}

bool MemoryConversationRepository::updateMemberRole(storage_detail::MemoryState &memory, ConversationId conversation, UserId user, MemberRole role)
{
    const auto found = memory.members.find({conversation, user});
    if (found == memory.members.end())
        return false;
    found->second.role = role;
    return true;
}

bool MemoryConversationRepository::updateMemberMute(storage_detail::MemoryState &memory, ConversationId conversation, UserId user, std::optional<TimePoint> mutedUntil)
{
    const auto found = memory.members.find({conversation, user});
    if (found == memory.members.end())
        return false;
    found->second.mutedUntil = mutedUntil;
    return true;
}

bool MemoryConversationRepository::removeMember(storage_detail::MemoryState &memory, ConversationId conversation, UserId user)
{
    memory.conversationPreferences.erase({conversation,user});
    return memory.members.erase({conversation, user}) == 1;
}

bool MemoryConversationRepository::updateDelivered(storage_detail::MemoryState &memory, ConversationId conversation, UserId user, std::uint64_t sequence)
{
    const auto found = memory.members.find({conversation, user});
    if (found == memory.members.end())
        return false;
    if (sequence <= found->second.lastDeliveredSequence)
        return false;
    found->second.lastDeliveredSequence =
        std::max(found->second.lastDeliveredSequence, sequence);
    return true;
}

bool MemoryConversationRepository::updateRead(storage_detail::MemoryState &memory, ConversationId conversation, UserId user, std::uint64_t sequence)
{
    const auto found = memory.members.find({conversation, user});
    if (found == memory.members.end())
        return false;
    if (sequence <= found->second.lastReadSequence)
        return false;
    found->second.lastReadSequence =
        std::max(found->second.lastReadSequence, sequence);
    found->second.lastDeliveredSequence =
        std::max(found->second.lastDeliveredSequence, sequence);
    return true;
}

}
