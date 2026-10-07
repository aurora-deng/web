#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace webserver::phase11
{

class InMemoryStore::Transaction final : public ITransaction
{
public:
    explicit Transaction(InMemoryStore &store)
        : store_(store), lock_(store.mutex_), snapshot_(store.state_)
    {
    }

    ~Transaction() override
    {
        if (!finished_)
            rollback();
    }

    void commit() override
    {
        if (finished_)
            throw std::logic_error("transaction already completed");
        finished_ = true;
        lock_.unlock();
    }

    void rollback() override
    {
        if (finished_)
            throw std::logic_error("transaction already completed");
        store_.state_ = std::move(snapshot_);
        finished_ = true;
        lock_.unlock();
    }

private:
    InMemoryStore &store_;
    std::unique_lock<std::recursive_mutex> lock_;
    State snapshot_;
    bool finished_ = false;
};

std::unique_ptr<ITransaction> InMemoryStore::beginTransaction()
{
    return std::make_unique<Transaction>(*this);
}

InMemoryStore::UserPair InMemoryStore::orderedUsers(UserId first, UserId second)
{
    return first < second ? UserPair{first, second} : UserPair{second, first};
}

User InMemoryStore::create(User user)
{
    std::lock_guard lock(mutex_);
    if (user.username.empty() || state_.usernames.contains(user.username))
        throw std::invalid_argument("username already exists or is empty");
    user.id = state_.nextUserId++;
    state_.usernames.emplace(user.username, user.id);
    state_.users.emplace(user.id, user);
    return user;
}

std::optional<User> InMemoryStore::findUser(UserId id) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.users.find(id);
    return found == state_.users.end() ? std::nullopt
                                       : std::optional<User>{found->second};
}

std::optional<User> InMemoryStore::findUserByName(std::string_view username) const
{
    std::lock_guard lock(mutex_);
    const auto named = state_.usernames.find(std::string(username));
    if (named == state_.usernames.end())
        return std::nullopt;
    return state_.users.at(named->second);
}

bool InMemoryStore::updateProfile(UserId id, std::string displayName,
                                  std::string biography)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.users.find(id);
    if (found == state_.users.end())
        return false;
    found->second.displayName = std::move(displayName);
    found->second.biography = std::move(biography);
    return true;
}

bool InMemoryStore::updateAvatar(UserId id, std::optional<FileId> avatar)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.users.find(id);
    if (found == state_.users.end() ||
        (avatar && !state_.files.contains(*avatar)))
        return false;
    found->second.avatarFileId = avatar;
    return true;
}

Session InMemoryStore::createSession(Session session)
{
    std::lock_guard lock(mutex_);
    if (session.id.empty() || session.tokenHash.empty() ||
        !state_.users.contains(session.userId) ||
        state_.sessionsById.contains(session.id) ||
        state_.sessionIdByTokenHash.contains(session.tokenHash))
        throw std::invalid_argument("invalid or duplicate session");
    state_.sessionIdByTokenHash.emplace(session.tokenHash, session.id);
    state_.sessionsById.emplace(session.id, session);
    return session;
}

std::optional<Session> InMemoryStore::findSessionByTokenHash(
    std::string_view tokenHash) const
{
    std::lock_guard lock(mutex_);
    const auto mapped = state_.sessionIdByTokenHash.find(std::string(tokenHash));
    if (mapped == state_.sessionIdByTokenHash.end())
        return std::nullopt;
    return state_.sessionsById.at(mapped->second);
}

bool InMemoryStore::revokeSession(std::string_view sessionId, TimePoint now)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.sessionsById.find(std::string(sessionId));
    if (found == state_.sessionsById.end())
        return false;
    found->second.revokedAt = now;
    return true;
}

FriendRequest InMemoryStore::createFriendRequest(FriendRequest request)
{
    std::lock_guard lock(mutex_);
    if (!state_.users.contains(request.senderId) ||
        !state_.users.contains(request.receiverId) ||
        request.senderId == request.receiverId)
        throw std::invalid_argument("invalid friend request users");
    request.id = state_.nextFriendRequestId++;
    state_.friendRequests.emplace(request.id, request);
    return request;
}

std::optional<FriendRequest> InMemoryStore::findFriendRequest(
    FriendRequestId id) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.friendRequests.find(id);
    return found == state_.friendRequests.end()
               ? std::nullopt
               : std::optional<FriendRequest>{found->second};
}

bool InMemoryStore::decideFriendRequest(FriendRequestId id, UserId receiver,
                                        FriendRequestState decision, TimePoint now)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.friendRequests.find(id);
    if (found == state_.friendRequests.end() ||
        found->second.receiverId != receiver ||
        found->second.state != FriendRequestState::Pending ||
        decision == FriendRequestState::Pending)
        return false;
    found->second.state = decision;
    found->second.decidedAt = now;
    if (decision == FriendRequestState::Accepted)
        state_.friendships.emplace(
            orderedUsers(found->second.senderId, found->second.receiverId));
    return true;
}

bool InMemoryStore::areFriends(UserId first, UserId second) const
{
    std::lock_guard lock(mutex_);
    return state_.friendships.contains(orderedUsers(first, second));
}

std::vector<UserId> InMemoryStore::listFriendIds(UserId user) const
{
    std::lock_guard lock(mutex_);
    std::vector<UserId> result;
    for (const auto &[low, high] : state_.friendships)
    {
        if (low == user)
            result.push_back(high);
        else if (high == user)
            result.push_back(low);
    }
    return result;
}

std::vector<FriendRequest> InMemoryStore::pendingRequests(UserId receiver) const
{
    std::lock_guard lock(mutex_);
    std::vector<FriendRequest> result;
    for (const auto &[_, request] : state_.friendRequests)
        if (request.receiverId == receiver &&
            request.state == FriendRequestState::Pending)
            result.push_back(request);
    return result;
}

bool InMemoryStore::removeFriendship(UserId first, UserId second)
{
    std::lock_guard lock(mutex_);
    const auto removed = state_.friendships.erase(orderedUsers(first, second)) != 0;
    if (removed)
    {
        state_.contactSettings.erase({first, second});
        state_.contactSettings.erase({second, first});
    }
    return removed;
}

ContactSettings InMemoryStore::contactSettings(UserId owner,
                                                UserId contact) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.contactSettings.find({owner, contact});
    return found == state_.contactSettings.end()
               ? ContactSettings{owner, contact, {}, false}
               : found->second;
}

bool InMemoryStore::updateContactSettings(UserId owner, UserId contact,
                                          std::string remark,
                                          bool notificationsMuted)
{
    std::lock_guard lock(mutex_);
    if (!state_.friendships.contains(orderedUsers(owner, contact)))
        return false;
    state_.contactSettings[{owner, contact}] =
        ContactSettings{owner, contact, std::move(remark), notificationsMuted};
    return true;
}

bool InMemoryStore::blockUser(UserId blocker, UserId blocked, TimePoint now)
{
    std::lock_guard lock(mutex_);
    if (blocker == blocked || !state_.users.contains(blocker) ||
        !state_.users.contains(blocked))
        return false;
    const auto inserted = state_.blocks.emplace(blocker, blocked).second;
    if (!inserted)
        return false;

    state_.friendships.erase(orderedUsers(blocker, blocked));
    state_.contactSettings.erase({blocker, blocked});
    state_.contactSettings.erase({blocked, blocker});
    for (auto &[_, request] : state_.friendRequests)
    {
        const bool samePair =
            (request.senderId == blocker && request.receiverId == blocked) ||
            (request.senderId == blocked && request.receiverId == blocker);
        if (samePair && request.state == FriendRequestState::Pending)
        {
            request.state = FriendRequestState::Rejected;
            request.decidedAt = now;
        }
    }
    return true;
}

bool InMemoryStore::unblockUser(UserId blocker, UserId blocked)
{
    std::lock_guard lock(mutex_);
    return state_.blocks.erase({blocker, blocked}) != 0;
}

bool InMemoryStore::isBlocked(UserId blocker, UserId blocked) const
{
    std::lock_guard lock(mutex_);
    return state_.blocks.contains({blocker, blocked});
}

bool InMemoryStore::blockedEitherDirection(UserId first, UserId second) const
{
    std::lock_guard lock(mutex_);
    return state_.blocks.contains({first, second}) ||
           state_.blocks.contains({second, first});
}

std::vector<UserId> InMemoryStore::listBlockedIds(UserId blocker) const
{
    std::lock_guard lock(mutex_);
    std::vector<UserId> result;
    for (const auto &[owner, blocked] : state_.blocks)
        if (owner == blocker)
            result.push_back(blocked);
    return result;
}

bool InMemoryStore::updateLastActive(UserId user, TimePoint now)
{
    std::lock_guard lock(mutex_);
    if (!state_.users.contains(user))
        return false;
    state_.lastActive[user] = now;
    return true;
}

std::optional<TimePoint> InMemoryStore::lastActive(UserId user) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.lastActive.find(user);
    return found == state_.lastActive.end()
               ? std::nullopt : std::optional<TimePoint>{found->second};
}

Conversation InMemoryStore::createConversation(
    Conversation conversation, std::vector<ConversationMember> members)
{
    std::lock_guard lock(mutex_);
    if (members.empty())
        throw std::invalid_argument("conversation requires members");
    conversation.id = state_.nextConversationId++;
    conversation.nextSequence = 1;
    state_.conversations.emplace(conversation.id, conversation);
    for (auto &member : members)
    {
        if (!state_.users.contains(member.userId))
            throw std::invalid_argument("conversation member does not exist");
        member.conversationId = conversation.id;
        state_.members.emplace(MemberKey{conversation.id, member.userId}, member);
    }
    return conversation;
}

std::optional<Conversation> InMemoryStore::findConversation(
    ConversationId id) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.conversations.find(id);
    return found == state_.conversations.end()
               ? std::nullopt
               : std::optional<Conversation>{found->second};
}

std::optional<ConversationMember> InMemoryStore::findMember(
    ConversationId conversation, UserId user) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.members.find({conversation, user});
    return found == state_.members.end()
               ? std::nullopt
               : std::optional<ConversationMember>{found->second};
}

std::optional<Conversation> InMemoryStore::findDirectConversation(
    UserId first, UserId second) const
{
    std::lock_guard lock(mutex_);
    for (const auto &[id, conversation] : state_.conversations)
    {
        if (conversation.kind != ConversationKind::Direct ||
            conversation.state != ConversationState::Active)
            continue;
        if (state_.members.contains({id, first}) &&
            state_.members.contains({id, second}))
            return conversation;
    }
    return std::nullopt;
}

std::vector<Conversation> InMemoryStore::listConversations(UserId user) const
{
    std::lock_guard lock(mutex_);
    std::vector<Conversation> result;
    for (const auto &[key, _] : state_.members)
        if (key.second == user)
        {
            const auto &conversation = state_.conversations.at(key.first);
            if (conversation.state == ConversationState::Active)
                result.push_back(conversation);
        }
    return result;
}

std::vector<ConversationSummary> InMemoryStore::listConversationSummaries(
    UserId user) const
{
    std::lock_guard lock(mutex_);
    std::vector<ConversationSummary> result;
    for (const auto &[key, member] : state_.members)
    {
        if (key.second != user)
            continue;
        const auto &conversation = state_.conversations.at(key.first);
        if (conversation.state != ConversationState::Active)
            continue;
        const auto latest = conversation.nextSequence > 0
                                ? conversation.nextSequence - 1 : 0;
        result.push_back({conversation,
                          member.lastDeliveredSequence,
                          member.lastReadSequence,
                          latest > member.lastReadSequence
                              ? latest - member.lastReadSequence : 0});
    }
    return result;
}

std::vector<ConversationMember> InMemoryStore::listMembers(
    ConversationId conversation) const
{
    std::lock_guard lock(mutex_);
    std::vector<ConversationMember> result;
    for (const auto &[key, member] : state_.members)
        if (key.first == conversation)
            result.push_back(member);
    return result;
}

bool InMemoryStore::addMember(ConversationMember member)
{
    std::lock_guard lock(mutex_);
    const auto conversation = state_.conversations.find(member.conversationId);
    if (conversation == state_.conversations.end() ||
        conversation->second.state != ConversationState::Active ||
        !state_.users.contains(member.userId))
        return false;
    return state_.members.emplace(
        MemberKey{member.conversationId, member.userId}, member).second;
}

bool InMemoryStore::updateConversationProfile(
    ConversationId conversation, std::string title,
    std::string announcement)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.conversations.find(conversation);
    if (found == state_.conversations.end() ||
        found->second.kind != ConversationKind::Group ||
        found->second.state != ConversationState::Active)
        return false;
    found->second.title = std::move(title);
    found->second.announcement = std::move(announcement);
    return true;
}

bool InMemoryStore::dissolveConversation(ConversationId conversation,
                                         UserId actor, TimePoint now)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.conversations.find(conversation);
    if (found == state_.conversations.end() ||
        found->second.kind != ConversationKind::Group ||
        found->second.state != ConversationState::Active)
        return false;
    found->second.state = ConversationState::Dissolved;
    found->second.dissolvedBy = actor;
    found->second.dissolvedAt = now;
    return true;
}

bool InMemoryStore::updateMemberRole(ConversationId conversation, UserId user,
                                     MemberRole role)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.members.find({conversation, user});
    if (found == state_.members.end())
        return false;
    found->second.role = role;
    return true;
}

bool InMemoryStore::updateMemberMute(
    ConversationId conversation, UserId user,
    std::optional<TimePoint> mutedUntil)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.members.find({conversation, user});
    if (found == state_.members.end())
        return false;
    found->second.mutedUntil = mutedUntil;
    return true;
}

bool InMemoryStore::removeMember(ConversationId conversation, UserId user)
{
    std::lock_guard lock(mutex_);
    return state_.members.erase({conversation, user}) == 1;
}

bool InMemoryStore::updateDelivered(ConversationId conversation, UserId user,
                                    std::uint64_t sequence)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.members.find({conversation, user});
    if (found == state_.members.end())
        return false;
    if (sequence <= found->second.lastDeliveredSequence)
        return false;
    found->second.lastDeliveredSequence =
        std::max(found->second.lastDeliveredSequence, sequence);
    return true;
}

bool InMemoryStore::updateRead(ConversationId conversation, UserId user,
                               std::uint64_t sequence)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.members.find({conversation, user});
    if (found == state_.members.end())
        return false;
    if (sequence <= found->second.lastReadSequence)
        return false;
    found->second.lastReadSequence =
        std::max(found->second.lastReadSequence, sequence);
    found->second.lastDeliveredSequence =
        std::max(found->second.lastDeliveredSequence, sequence);
    return true;
}

AppendMessageResult InMemoryStore::appendMessage(Message message)
{
    std::lock_guard lock(mutex_);
    const IdempotencyKey key{
        message.conversationId, message.senderId, message.clientMessageId};
    if (message.clientMessageId.empty())
        throw std::invalid_argument("client message id is required");
    if (const auto duplicate = state_.idempotency.find(key);
        duplicate != state_.idempotency.end())
        return {hydrateMessageLocked(state_.messages.at(duplicate->second)), false};

    const auto conversation = state_.conversations.find(message.conversationId);
    if (conversation == state_.conversations.end())
        throw std::invalid_argument("conversation does not exist");
    if (conversation->second.state != ConversationState::Active)
        throw std::invalid_argument("conversation is not active");
    message.id = state_.nextMessageId++;
    message.sequence = conversation->second.nextSequence++;
    state_.messages.emplace(message.id, message);
    state_.messageIds[message.conversationId].push_back(message.id);
    state_.idempotency.emplace(key, message.id);
    return {message, true};
}

Message InMemoryStore::hydrateMessageLocked(Message message) const
{
    message.attachments.clear();
    const auto linked = state_.attachments.find(message.id);
    if (linked == state_.attachments.end()) return message;
    for (const auto fileId : linked->second)
    {
        const auto file = state_.files.find(fileId);
        if (file == state_.files.end() || file->second.state != FileState::Ready)
            continue;
        message.attachments.push_back(
            {fileId, file->second.originalName, file->second.mediaType,
             file->second.expectedSize, file->second.sha256});
    }
    return message;
}

std::vector<Message> InMemoryStore::messagesAfter(
    ConversationId conversation, std::uint64_t sequence,
    std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    std::vector<Message> result;
    const auto found = state_.messageIds.find(conversation);
    if (found == state_.messageIds.end())
        return result;
    for (const auto id : found->second)
    {
        const auto &message = state_.messages.at(id);
        if (message.sequence > sequence)
        {
            result.push_back(hydrateMessageLocked(message));
            if (result.size() == limit)
                break;
        }
    }
    return result;
}

std::vector<Message> InMemoryStore::searchMessages(
    ConversationId conversation, std::string_view query,
    std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    std::vector<Message> result;
    const auto found = state_.messageIds.find(conversation);
    if (found == state_.messageIds.end() || query.empty())
        return result;
    for (const auto id : found->second)
    {
        const auto &message = state_.messages.at(id);
        if (message.state == MessageState::Active &&
            message.body.find(query) != std::string::npos)
        {
            result.push_back(hydrateMessageLocked(message));
            if (result.size() == limit)
                break;
        }
    }
    return result;
}

std::optional<Message> InMemoryStore::findMessage(MessageId id) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.messages.find(id);
    return found == state_.messages.end()
               ? std::nullopt
               : std::optional<Message>{hydrateMessageLocked(found->second)};
}

RecallMessageResult InMemoryStore::recallMessage(MessageId id, UserId actor,
                                                 TimePoint now)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.messages.find(id);
    if (found == state_.messages.end())
        return {};
    if (found->second.state == MessageState::Recalled)
        return {hydrateMessageLocked(found->second), false};
    found->second.state = MessageState::Recalled;
    found->second.recalledBy = actor;
    found->second.recalledAt = now;
    return {hydrateMessageLocked(found->second), true};
}

FileObject InMemoryStore::createFile(FileObject file)
{
    std::lock_guard lock(mutex_);
    if (!state_.users.contains(file.ownerId) || file.expectedSize == 0)
        throw std::invalid_argument("invalid file metadata");
    file.id = state_.nextFileId++;
    state_.files.emplace(file.id, file);
    return file;
}

std::optional<FileObject> InMemoryStore::findFile(FileId id) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.files.find(id);
    return found == state_.files.end() ? std::nullopt
                                      : std::optional<FileObject>{found->second};
}

bool InMemoryStore::updateFileProgress(FileId id, UserId owner,
                                       std::uint64_t expectedOffset,
                                       std::uint64_t uploadedSize)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.files.find(id);
    if (found == state_.files.end() || found->second.ownerId != owner ||
        found->second.state != FileState::Pending ||
        found->second.uploadedSize != expectedOffset ||
        uploadedSize < expectedOffset || uploadedSize > found->second.expectedSize)
        return false;
    found->second.uploadedSize = uploadedSize;
    return true;
}

bool InMemoryStore::completeFile(FileId id, UserId owner,
                                 std::string sha256, TimePoint now)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.files.find(id);
    if (found == state_.files.end() || found->second.ownerId != owner ||
        found->second.state != FileState::Pending ||
        found->second.uploadedSize != found->second.expectedSize ||
        sha256.size() != 64)
        return false;
    found->second.sha256 = std::move(sha256);
    found->second.state = FileState::Ready;
    found->second.readyAt = now;
    return true;
}

bool InMemoryStore::failFile(FileId id, UserId owner)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.files.find(id);
    if (found == state_.files.end() || found->second.ownerId != owner ||
        found->second.state != FileState::Pending)
        return false;
    found->second.state = FileState::Failed;
    return true;
}

bool InMemoryStore::attachFile(MessageId message, FileId file)
{
    std::lock_guard lock(mutex_);
    const auto foundMessage = state_.messages.find(message);
    const auto foundFile = state_.files.find(file);
    if (foundMessage == state_.messages.end() || foundFile == state_.files.end() ||
        foundFile->second.state != FileState::Ready ||
        foundMessage->second.senderId != foundFile->second.ownerId)
        return false;
    auto &linked = state_.attachments[message];
    if (!linked.empty()) return false;
    linked.push_back(file);
    return true;
}

std::vector<MessageAttachment> InMemoryStore::attachmentsForMessage(
    MessageId message) const
{
    std::lock_guard lock(mutex_);
    Message stub;
    stub.id = message;
    return hydrateMessageLocked(std::move(stub)).attachments;
}

bool InMemoryStore::canAccessFile(UserId user, FileId file) const
{
    std::lock_guard lock(mutex_);
    const auto foundFile = state_.files.find(file);
    if (foundFile == state_.files.end() ||
        foundFile->second.state != FileState::Ready)
        return false;
    if (foundFile->second.ownerId == user) return true;
    for (const auto &[_, profile] : state_.users)
        if (profile.avatarFileId == file) return true;
    for (const auto &[messageId, linked] : state_.attachments)
    {
        if (std::find(linked.begin(), linked.end(), file) == linked.end()) continue;
        const auto message = state_.messages.find(messageId);
        if (message != state_.messages.end() &&
            state_.members.contains({message->second.conversationId, user}))
            return true;
    }
    return false;
}

OutboxEvent InMemoryStore::appendOutbox(OutboxEvent event)
{
    std::lock_guard lock(mutex_);
    event.id = state_.nextOutboxEventId++;
    state_.outbox.emplace(event.id, event);
    return event;
}

std::vector<OutboxEvent> InMemoryStore::pendingOutbox(std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    std::vector<OutboxEvent> result;
    for (const auto &[_, event] : state_.outbox)
    {
        if (!event.publishedAt)
            result.push_back(event);
    }
    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right) {
                  return left.id < right.id;
              });
    if (result.size() > limit)
        result.resize(limit);
    return result;
}

bool InMemoryStore::markPublished(OutboxEventId id, TimePoint now)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.outbox.find(id);
    if (found == state_.outbox.end())
        return false;
    found->second.publishedAt = now;
    return true;
}

ModelNode InMemoryStore::createModelNode(ModelNode node)
{
    std::lock_guard lock(mutex_);
    node.id = state_.nextModelNodeId++;
    state_.modelNodes.emplace(node.id, node);
    return node;
}

ModelVersion InMemoryStore::createModelVersion(ModelVersion version)
{
    std::lock_guard lock(mutex_);
    if (!state_.modelNodes.contains(version.nodeId))
        throw std::invalid_argument("model node does not exist");
    version.id = state_.nextModelVersionId++;
    state_.modelVersions.emplace(version.id, version);
    return version;
}

ModelEdge InMemoryStore::createModelEdge(ModelEdge edge)
{
    std::lock_guard lock(mutex_);
    if (edge.from == edge.to || !state_.modelNodes.contains(edge.from) ||
        !state_.modelNodes.contains(edge.to))
        throw std::invalid_argument("invalid model edge");
    edge.id = state_.nextModelEdgeId++;
    state_.modelEdges.emplace(edge.id, edge);
    return edge;
}

bool InMemoryStore::bindAiAccount(ModelBinding binding)
{
    std::lock_guard lock(mutex_);
    const auto user = state_.users.find(binding.aiUserId);
    if (user == state_.users.end() || !user->second.aiAccount ||
        !state_.modelNodes.contains(binding.nodeId))
        return false;
    state_.modelBindings.insert_or_assign(binding.aiUserId, binding);
    return true;
}

std::optional<ModelNode> InMemoryStore::findModelNode(ModelNodeId id) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.modelNodes.find(id);
    return found == state_.modelNodes.end()
               ? std::nullopt
               : std::optional<ModelNode>{found->second};
}

std::optional<ModelVersion> InMemoryStore::findModelVersion(
    ModelVersionId id) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.modelVersions.find(id);
    return found == state_.modelVersions.end()
               ? std::nullopt
               : std::optional<ModelVersion>{found->second};
}

std::optional<ModelBinding> InMemoryStore::findModelBinding(UserId aiUser) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.modelBindings.find(aiUser);
    return found == state_.modelBindings.end()
               ? std::nullopt
               : std::optional<ModelBinding>{found->second};
}

std::vector<ModelEdge> InMemoryStore::modelEdges() const
{
    std::lock_guard lock(mutex_);
    std::vector<ModelEdge> result;
    result.reserve(state_.modelEdges.size());
    for (const auto &[_, edge] : state_.modelEdges)
        result.push_back(edge);
    return result;
}

bool InMemoryStore::activateModelVersion(ModelNodeId node, ModelVersionId version,
                                         ModelActivationAudit audit)
{
    std::lock_guard lock(mutex_);
    const auto foundNode = state_.modelNodes.find(node);
    const auto foundVersion = state_.modelVersions.find(version);
    if (foundNode == state_.modelNodes.end() ||
        foundVersion == state_.modelVersions.end() ||
        foundVersion->second.nodeId != node ||
        foundVersion->second.checksum.empty() ||
        foundVersion->second.state == ModelVersionState::Rejected ||
        audit.nodeId != node || audit.targetVersionId != version ||
        audit.outcome != ModelActivationOutcome::Succeeded ||
        audit.actor.empty())
        return false;
    audit.previousVersionId = foundNode->second.activeVersionId;
    if (foundNode->second.activeVersionId)
    {
        auto &previous = state_.modelVersions.at(*foundNode->second.activeVersionId);
        previous.state = ModelVersionState::Retired;
    }
    foundVersion->second.state = ModelVersionState::Active;
    foundNode->second.activeVersionId = version;
    audit.id = state_.nextModelActivationAuditId++;
    state_.modelActivationAudits.emplace(audit.id, std::move(audit));
    return true;
}

ModelActivationAudit InMemoryStore::appendModelActivationAudit(
    ModelActivationAudit audit)
{
    std::lock_guard lock(mutex_);
    const auto version = state_.modelVersions.find(audit.targetVersionId);
    if (!state_.modelNodes.contains(audit.nodeId) ||
        version == state_.modelVersions.end() ||
        version->second.nodeId != audit.nodeId || audit.actor.empty())
        throw std::invalid_argument("invalid model activation audit");
    audit.id = state_.nextModelActivationAuditId++;
    state_.modelActivationAudits.emplace(audit.id, audit);
    return audit;
}

std::vector<ModelActivationAudit> InMemoryStore::modelActivationAudits(
    ModelNodeId node, std::size_t limit) const
{
    std::lock_guard lock(mutex_);
    std::vector<ModelActivationAudit> result;
    for (const auto &[_, audit] : state_.modelActivationAudits)
        if (audit.nodeId == node)
            result.push_back(audit);
    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right)
              {
                  return left.createdAt != right.createdAt
                             ? left.createdAt > right.createdAt
                             : left.id > right.id;
              });
    if (result.size() > limit)
        result.resize(limit);
    return result;
}

bool InMemoryStore::upsertGenerationAudit(AiGenerationAudit audit)
{
    std::lock_guard lock(mutex_);
    if (audit.generationId.empty() || audit.promptSha256.size() != 64 ||
        audit.revision == 0)
        return false;
    const auto found = state_.generationAudits.find(audit.generationId);
    if (found != state_.generationAudits.end() &&
        found->second.revision > audit.revision)
        return false;
    state_.generationAudits.insert_or_assign(audit.generationId,
                                             std::move(audit));
    return true;
}

std::optional<AiGenerationAudit> InMemoryStore::findGenerationAudit(
    std::string_view generationId) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.generationAudits.find(std::string(generationId));
    return found == state_.generationAudits.end()
               ? std::nullopt
               : std::optional<AiGenerationAudit>{found->second};
}

TrainingCandidate InMemoryStore::createTrainingCandidate(
    TrainingCandidate candidate)
{
    std::lock_guard lock(mutex_);
    candidate.id = state_.nextTrainingCandidateId++;
    state_.trainingCandidates.emplace(candidate.id, candidate);
    return candidate;
}

std::optional<TrainingCandidate> InMemoryStore::findTrainingCandidate(
    TrainingCandidateId id) const
{
    std::lock_guard lock(mutex_);
    const auto found = state_.trainingCandidates.find(id);
    return found == state_.trainingCandidates.end()
               ? std::nullopt
               : std::optional<TrainingCandidate>{found->second};
}

bool InMemoryStore::updateTrainingCandidate(TrainingCandidate candidate)
{
    std::lock_guard lock(mutex_);
    const auto found = state_.trainingCandidates.find(candidate.id);
    if (found == state_.trainingCandidates.end())
        return false;
    found->second = std::move(candidate);
    return true;
}

DatasetVersion InMemoryStore::createDatasetVersion(DatasetVersion version)
{
    std::lock_guard lock(mutex_);
    version.id = state_.nextDatasetVersionId++;
    state_.datasets.emplace(version.id, version);
    return version;
}

} // namespace webserver::phase11
