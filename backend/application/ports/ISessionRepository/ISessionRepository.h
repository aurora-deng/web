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
class ISessionRepository
{
public:
    virtual ~ISessionRepository() = default;
    virtual Session createSession(Session session) = 0;
    [[nodiscard]] virtual std::optional<Session> findSessionByTokenHash(
        std::string_view tokenHash) const = 0;
    virtual bool revokeSession(std::string_view sessionId, TimePoint now) = 0;
};
}
