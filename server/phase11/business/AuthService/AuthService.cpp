#include "server/phase11/business/AuthService/AuthService.h"

#include <algorithm>
#include <cctype>

namespace webserver::phase11
{

AuthService::AuthService(IUserRepository &users,
                         ISessionRepository &sessions,
                         ICredentialCodec &credentials,
                         ILoginRateLimiter &rateLimiter,
                         std::chrono::seconds lifetime)
    : users_(users), sessions_(sessions), credentials_(credentials),
      rateLimiter_(rateLimiter), lifetime_(lifetime)
{
    if (lifetime_ <= std::chrono::seconds::zero())
        throw std::invalid_argument("session lifetime must be positive");
}

std::string AuthService::normalizeUsername(std::string_view username)
{
    std::string normalized;
    normalized.reserve(username.size());
    for (const unsigned char c : username)
    {
        if (!(std::isalnum(c) || c == '_' || c == '-'))
            throw ApplicationError(ErrorCode::InvalidArgument,
                                   "username contains unsupported characters");
        normalized.push_back(static_cast<char>(std::tolower(c)));
    }
    if (normalized.size() < 3 || normalized.size() > 32)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "username length must be between 3 and 32");
    return normalized;
}

User AuthService::registerUser(std::string username,
                               std::string password,
                               std::string displayName,
                               TimePoint now)
{
    username = normalizeUsername(username);
    if (password.size() < 8 || password.size() > 256)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "password length must be between 8 and 256");
    if (displayName.empty())
        displayName = username;
    if (displayName.size() > 64)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "display name is too long");
    if (users_.findUserByName(username))
        throw ApplicationError(ErrorCode::Conflict, "username already exists");

    User user;
    user.username = std::move(username);
    user.displayName = std::move(displayName);
    user.passwordHash = credentials_.hashPassword(password);
    user.createdAt = now;
    return users_.create(std::move(user));
}

LoginSession AuthService::login(std::string_view username,
                                std::string_view password,
                                TimePoint now,
                                std::string_view origin)
{
    const auto normalized = normalizeUsername(username);
    if (!rateLimiter_.allowLogin(normalized, origin, now))
        throw ApplicationError(ErrorCode::RateLimited,
                               "too many failed login attempts");
    const auto user = users_.findUserByName(normalized);
    const auto &encoded = user ? user->passwordHash
                               : credentials_.dummyPasswordHash();
    const bool passwordAccepted = credentials_.verifyPassword(encoded, password);
    // 不区分“用户不存在”和“密码错误”，防止登录接口被用于枚举账号。
    // 即使账号不存在也执行一次相同类型的密码校验，减少明显的时间差。
    if (!user || user->disabled || !passwordAccepted)
    {
        rateLimiter_.recordFailure(normalized, origin, now);
        throw ApplicationError(ErrorCode::Unauthenticated,
                               "invalid username or password");
    }
    rateLimiter_.recordSuccess(normalized, origin, now);

    const auto sessionToken = credentials_.randomToken();
    // CSRF Token 像 Session Token 的“专用副钥匙”：使用持久服务器密钥和独立域标签
    // 派生。浏览器刷新后可重新取得同一个值，但仅凭 CSRF Token 无法还原 Session Token。
    const auto csrfToken = deriveCsrfToken(sessionToken);
    Session session;
    session.id = credentials_.randomToken();
    session.userId = user->id;
    session.tokenHash = credentials_.hashToken(sessionToken);
    session.csrfHash = credentials_.hashToken(csrfToken);
    session.createdAt = now;
    session.expiresAt = now + lifetime_;
    sessions_.createSession(std::move(session));
    return {*user, sessionToken, csrfToken, now + lifetime_};
}

std::optional<User> AuthService::authenticate(
    std::string_view sessionToken, TimePoint now) const
{
    const auto restored = restoreSession(sessionToken, now);
    if (!restored)
        return std::nullopt;
    return restored->user;
}

std::optional<RestoredSession> AuthService::restoreSession(
    std::string_view sessionToken, TimePoint now) const
{
    if (sessionToken.empty())
        return std::nullopt;
    const auto session = sessions_.findSessionByTokenHash(
        credentials_.hashToken(sessionToken));
    if (!session || session->revokedAt || now >= session->expiresAt)
        return std::nullopt;
    const auto user = users_.findUser(session->userId);
    if (!user || user->disabled)
        return std::nullopt;
    return RestoredSession{*user, deriveCsrfToken(sessionToken),
                           session->expiresAt};
}

bool AuthService::verifyCsrf(std::string_view sessionToken,
                             std::string_view csrfToken,
                             TimePoint now) const
{
    if (!authenticate(sessionToken, now))
        return false;
    const auto session = sessions_.findSessionByTokenHash(
        credentials_.hashToken(sessionToken));
    return session && credentials_.verifyToken(session->csrfHash, csrfToken);
}

void AuthService::logout(std::string_view sessionToken, TimePoint now)
{
    const auto session = sessions_.findSessionByTokenHash(
        credentials_.hashToken(sessionToken));
    if (session)
        sessions_.revokeSession(session->id, now);
}

User AuthService::updateProfile(UserId user, std::string displayName,
                                std::string biography)
{
    if (displayName.empty() || displayName.size() > 64 || biography.size() > 512)
        throw ApplicationError(ErrorCode::InvalidArgument,
                               "invalid profile field length");
    if (!users_.updateProfile(user, std::move(displayName), std::move(biography)))
        throw ApplicationError(ErrorCode::NotFound, "user not found");
    return *users_.findUser(user);
}

std::string AuthService::deriveCsrfToken(std::string_view sessionToken) const
{
    // 域分离标签使该值与数据库保存的 Session Token 指纹用途不同。
    std::string material{"phase11-csrf-v1:"};
    material.append(sessionToken);
    return credentials_.hashToken(material);
}

} // namespace webserver::phase11
