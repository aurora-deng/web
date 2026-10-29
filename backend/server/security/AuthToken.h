#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace webserver::security
{

/**
 * @brief 跨 HTTP、WebSocket、SSE 与 gRPC 共用的可信身份。
 *
 * userId 是全局唯一用户主键，tenant 是授权域标签。二者都来自服务端签名的令牌，
 * 不能再从客户端随意填写的 ?uid= 参数直接信任。当前会话目录仍按全局 userId
 * 路由；若业务使用“每租户从 1 重新编号”的主键，必须先改为复合路由键。
 */
struct AuthIdentity
{
    std::uint64_t userId = 0;
    std::string tenant;
    std::uint64_t expiresAtUnixSeconds = 0;
};

enum class AuthError
{
    None,
    Missing,
    Malformed,
    BadSignature,
    Expired,
    InvalidClaims
};

struct AuthResult
{
    std::optional<AuthIdentity> identity;
    AuthError error = AuthError::Missing;

    explicit operator bool() const noexcept { return identity.has_value(); }
};

/**
 * @brief HMAC-SHA256 身份令牌签发与校验器。
 *
 * 线格式为 v1.userId.expiry.tenant.signatureHex。签名覆盖前四段；tenant 只允许
 * 字母、数字、下划线和短横线，避免分隔符歧义。生产模式要求至少 32 字节密钥。
 */
class AuthToken
{
public:
    explicit AuthToken(std::string secret);

    [[nodiscard]] bool enabled() const noexcept { return !secret_.empty(); }
    [[nodiscard]] const std::string &secret() const noexcept { return secret_; }

    [[nodiscard]] std::string issue(const AuthIdentity &identity) const;
    [[nodiscard]] AuthResult verify(
        std::string_view token,
        std::chrono::system_clock::time_point now =
            std::chrono::system_clock::now()) const;

    /** 从 Authorization: Bearer 或 Cookie: web_session= 中提取并校验。 */
    [[nodiscard]] AuthResult authenticateHeaders(
        const std::unordered_map<std::string, std::string> &headers,
        std::chrono::system_clock::time_point now =
            std::chrono::system_clock::now()) const;

    // 返回拥有自身存储的字符串。调用者即使传入临时 std::string，结果也不会
    // 变成悬空 string_view；认证本身包含 HMAC，优先保证接口生命周期安全。
    [[nodiscard]] static std::optional<std::string> bearerToken(
        std::string_view authorization);
    [[nodiscard]] static std::optional<std::string> cookieToken(
        std::string_view cookieHeader,
        std::string_view cookieName = "web_session");

private:
    [[nodiscard]] std::string signature(std::string_view payload) const;
    [[nodiscard]] static bool validTenant(std::string_view tenant);

    std::string secret_;
};

} // namespace webserver::security
