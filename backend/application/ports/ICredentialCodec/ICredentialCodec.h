#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class ICredentialCodec
{
public:
    virtual ~ICredentialCodec() = default;
    [[nodiscard]] virtual std::string hashPassword(std::string_view password) const = 0;
    [[nodiscard]] virtual std::string dummyPasswordHash() const = 0;
    [[nodiscard]] virtual bool verifyPassword(std::string_view encoded,
                                              std::string_view password) const = 0;
    [[nodiscard]] virtual std::string randomToken() const = 0;
    [[nodiscard]] virtual std::string hashToken(std::string_view token) const = 0;
    [[nodiscard]] virtual bool verifyToken(std::string_view encoded,
                                           std::string_view token) const = 0;
};
}
