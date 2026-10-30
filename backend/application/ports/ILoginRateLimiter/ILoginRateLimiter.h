#pragma once
#include "backend/application/domain/IdentityTypes/IdentityTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class ILoginRateLimiter
{
public:
    virtual ~ILoginRateLimiter() = default;
    [[nodiscard]] virtual bool allowLogin(std::string_view username,
                                          std::string_view origin,
                                          TimePoint now) = 0;
    virtual void recordFailure(std::string_view username,
                               std::string_view origin,
                               TimePoint now) = 0;
    virtual void recordSuccess(std::string_view username,
                               std::string_view origin,
                               TimePoint now) = 0;
};
}
