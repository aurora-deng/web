#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/domain/AccountTypes/AccountTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IUserRepository
{
public:
    virtual ~IUserRepository() = default;
    virtual User create(User user) = 0;
    [[nodiscard]] virtual std::optional<User> findUser(UserId id) const = 0;
    [[nodiscard]] virtual std::optional<User> findUserByName(
        std::string_view username) const = 0;
    virtual bool updateProfile(UserId id, std::string displayName,
                               std::string biography) = 0;
    virtual bool updateAvatar(UserId id, std::optional<FileId> avatar) = 0;
};
}
