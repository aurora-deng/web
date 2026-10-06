#pragma once

#include "server/phase11/business/ApplicationError/ApplicationError.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/ports/Ports/Ports.h"

#include <chrono>

namespace webserver::phase11
{

/** Session Token 仍只存在于 HttpOnly Cookie，业务接口只返回安全的会话结果。 */
class AuthService final : public IAuthBusiness
{
public:
    AuthService(IUserRepository &users,
                ISessionRepository &sessions,
                ICredentialCodec &credentials,
                ILoginRateLimiter &rateLimiter,
                std::chrono::seconds lifetime = std::chrono::hours(24));

    User registerUser(std::string username,
                      std::string password,
                      std::string displayName,
                      TimePoint now) override;
    LoginSession login(std::string_view username,
                       std::string_view password,
                       TimePoint now,
                       std::string_view origin = {}) override;
    [[nodiscard]] std::optional<RestoredSession> restoreSession(
        std::string_view sessionToken, TimePoint now) const override;
    std::optional<User> authenticate(std::string_view sessionToken,
                                     TimePoint now) const override;
    bool verifyCsrf(std::string_view sessionToken,
                    std::string_view csrfToken,
                    TimePoint now) const override;
    void logout(std::string_view sessionToken, TimePoint now) override;
    User updateProfile(UserId user, std::string displayName,
                       std::string biography) override;

    static std::string normalizeUsername(std::string_view username);

private:
    IUserRepository &users_;
    ISessionRepository &sessions_;
    ICredentialCodec &credentials_;
    ILoginRateLimiter &rateLimiter_;
    std::chrono::seconds lifetime_;

    [[nodiscard]] std::string deriveCsrfToken(
        std::string_view sessionToken) const;
};

} // namespace webserver::phase11
