#include "backend/application/storage/memory/MemoryAccountRepository/MemoryAccountRepository.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
namespace webserver::phase11 {
using storage_detail::orderedUsers;
using storage_detail::UserPair;
using storage_detail::MemberKey;
using storage_detail::IdempotencyKey;
User MemoryAccountRepository::create(storage_detail::MemoryState &memory, User user)
{
    if (user.username.empty() || memory.usernames.contains(user.username))
        throw std::invalid_argument("username already exists or is empty");
    user.id = memory.nextUserId++;
    memory.usernames.emplace(user.username, user.id);
    memory.users.emplace(user.id, user);
    return user;
}

std::optional<User> MemoryAccountRepository::findUser(const storage_detail::MemoryState &memory, UserId id)
{
    const auto found = memory.users.find(id);
    return found == memory.users.end() ? std::nullopt
                                       : std::optional<User>{found->second};
}

std::optional<User> MemoryAccountRepository::findUserByName(const storage_detail::MemoryState &memory, std::string_view username)
{
    const auto named = memory.usernames.find(std::string(username));
    if (named == memory.usernames.end())
        return std::nullopt;
    return memory.users.at(named->second);
}

bool MemoryAccountRepository::updateProfile(storage_detail::MemoryState &memory, UserId id, std::string displayName, std::string biography)
{
    const auto found = memory.users.find(id);
    if (found == memory.users.end())
        return false;
    found->second.displayName = std::move(displayName);
    found->second.biography = std::move(biography);
    return true;
}

ProfileWriteResult MemoryAccountRepository::writeProfileOnce(storage_detail::MemoryState &memory, UserId id, std::string operation, std::string displayName, std::string biography)
{const auto user=memory.users.find(id);if(user==memory.users.end())return ProfileWriteResult::NotFound;
    const auto key=std::make_pair(id,operation);const auto payload=std::make_pair(displayName,biography);
    if(const auto previous=memory.profileOperations.find(key);previous!=memory.profileOperations.end())return previous->second==payload?ProfileWriteResult::Replay:ProfileWriteResult::Conflict;
    memory.profileOperations.emplace(key,payload);user->second.displayName=std::move(displayName);user->second.biography=std::move(biography);return ProfileWriteResult::Applied;
}

bool MemoryAccountRepository::updateAvatar(storage_detail::MemoryState &memory, UserId id, std::optional<FileId> avatar)
{
    const auto found = memory.users.find(id);
    if (found == memory.users.end() ||
        (avatar && !memory.files.contains(*avatar)))
        return false;
    found->second.avatarFileId = avatar;
    return true;
}

Session MemoryAccountRepository::createSession(storage_detail::MemoryState &memory, Session session)
{
    if (session.id.empty() || session.tokenHash.empty() ||
        !memory.users.contains(session.userId) ||
        memory.sessionsById.contains(session.id) ||
        memory.sessionIdByTokenHash.contains(session.tokenHash))
        throw std::invalid_argument("invalid or duplicate session");
    memory.sessionIdByTokenHash.emplace(session.tokenHash, session.id);
    memory.sessionsById.emplace(session.id, session);
    return session;
}

std::optional<Session> MemoryAccountRepository::findSessionByTokenHash(const storage_detail::MemoryState &memory, std::string_view tokenHash)
{
    const auto mapped = memory.sessionIdByTokenHash.find(std::string(tokenHash));
    if (mapped == memory.sessionIdByTokenHash.end())
        return std::nullopt;
    return memory.sessionsById.at(mapped->second);
}

bool MemoryAccountRepository::revokeSession(storage_detail::MemoryState &memory, std::string_view sessionId, TimePoint now)
{
    const auto found = memory.sessionsById.find(std::string(sessionId));
    if (found == memory.sessionsById.end())
        return false;
    found->second.revokedAt = now;
    return true;
}

}
