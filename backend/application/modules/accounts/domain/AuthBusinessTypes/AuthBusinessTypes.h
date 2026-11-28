#pragma once
#include "backend/application/modules/accounts/domain/AccountTypes/AccountTypes.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

struct LoginSession
{
    User user;
    std::string sessionToken;
    std::string csrfToken;
    TimePoint expiresAt{};
};

struct RestoredSession
{
    User user;
    std::string csrfToken;
    TimePoint expiresAt{};
};

} // namespace webserver::phase11
