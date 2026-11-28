#pragma once
#include "backend/application/modules/accounts/domain/AuthBusinessTypes/AuthBusinessTypes.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

class IAuthBusiness
{
public:
    virtual ~IAuthBusiness() = default;

    virtual User registerUser(std::string username, std::string password,
                              std::string displayName, TimePoint now) = 0;
    virtual LoginSession login(std::string_view username,
                               std::string_view password, TimePoint now,
                               std::string_view origin = {}) = 0;
    [[nodiscard]] virtual std::optional<RestoredSession> restoreSession(
        std::string_view sessionToken, TimePoint now) const = 0;
    [[nodiscard]] virtual std::optional<User> authenticate(
        std::string_view sessionToken, TimePoint now) const = 0;
    [[nodiscard]] virtual bool verifyCsrf(std::string_view sessionToken,
                                          std::string_view csrfToken,
                                          TimePoint now) const = 0;
    virtual void logout(std::string_view sessionToken, TimePoint now) = 0;
    virtual User updateProfile(UserId user, std::string displayName,
                               std::string biography) = 0;
    virtual User updateProfileOnce(UserId user,std::string operation,std::string displayName,std::string biography)=0;
};

} // namespace webserver::phase11
