#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"

namespace webserver::phase11 {
/** AccountTypes: protocol/database independent business facts. */
struct User
{
    UserId id = 0;
    std::string username;
    std::string displayName;
    std::string biography;
    std::string passwordHash;
    bool disabled = false;
    bool aiAccount = false;
    TimePoint createdAt{};
    std::optional<FileId> avatarFileId;
};

struct Session
{
    std::string id;
    UserId userId = 0;
    std::string tokenHash;
    std::string csrfHash;
    TimePoint createdAt{};
    TimePoint expiresAt{};
    std::optional<TimePoint> revokedAt;
};
}
