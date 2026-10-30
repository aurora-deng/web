#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/AccountTypes/AccountTypes.h"

namespace webserver::phase11 {
/** SocialTypes: protocol/database independent business facts. */
enum class FriendRequestState
{
    Pending,
    Accepted,
    Rejected
};

struct FriendRequest
{
    FriendRequestId id = 0;
    UserId senderId = 0;
    UserId receiverId = 0;
    FriendRequestState state = FriendRequestState::Pending;
    TimePoint createdAt{};
    std::optional<TimePoint> decidedAt;
};

struct ContactSettings
{
    UserId ownerId = 0;
    UserId contactId = 0;
    std::string remark;
    bool notificationsMuted = false;
};

struct FriendContact
{
    User user;
    ContactSettings settings;
    std::optional<TimePoint> lastActiveAt;
};
}
