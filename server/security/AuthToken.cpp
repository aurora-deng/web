#include "server/security/AuthToken.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <vector>

namespace webserver::security
{
namespace
{
std::string_view trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

bool iequals(std::string_view left, std::string_view right)
{
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](char a, char b)
                      {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

template <typename Integer>
bool parseUnsigned(std::string_view text, Integer &value)
{
    if (text.empty())
        return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::vector<std::string_view> split(std::string_view value, char delimiter)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= value.size())
    {
        const auto end = value.find(delimiter, start);
        parts.push_back(value.substr(
            start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return parts;
}
} // namespace

AuthToken::AuthToken(std::string secret) : secret_(std::move(secret))
{
    if (!secret_.empty() && secret_.size() < 16)
        throw std::invalid_argument("authentication secret must contain at least 16 bytes");
    // OpenSSL 旧版 HMAC API 的密钥长度参数是 int。这里同时设置合理上限，
    // 避免超长环境变量在转换时溢出，也避免错误配置白白占用大量内存。
    if (secret_.size() > 4096)
        throw std::invalid_argument("authentication secret must not exceed 4096 bytes");
}

bool AuthToken::validTenant(std::string_view tenant)
{
    if (tenant.empty() || tenant.size() > 64)
        return false;
    return std::all_of(tenant.begin(), tenant.end(), [](char value)
    {
        const auto byte = static_cast<unsigned char>(value);
        return std::isalnum(byte) != 0 || value == '_' || value == '-';
    });
}

std::string AuthToken::signature(std::string_view payload) const
{
    if (secret_.empty())
        return {};

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), secret_.data(), static_cast<int>(secret_.size()),
              reinterpret_cast<const unsigned char *>(payload.data()), payload.size(),
              digest.data(), &length))
        throw std::runtime_error("HMAC-SHA256 failed");

    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(length * 2);
    for (unsigned int index = 0; index < length; ++index)
    {
        encoded.push_back(hex[digest[index] >> 4U]);
        encoded.push_back(hex[digest[index] & 0x0FU]);
    }
    return encoded;
}

std::string AuthToken::issue(const AuthIdentity &identity) const
{
    if (!enabled())
        throw std::logic_error("cannot issue a token without an authentication secret");
    if (identity.userId == 0 || identity.expiresAtUnixSeconds == 0 ||
        !validTenant(identity.tenant))
        throw std::invalid_argument("invalid authentication claims");

    std::string payload = "v1." + std::to_string(identity.userId) + "." +
                          std::to_string(identity.expiresAtUnixSeconds) + "." +
                          identity.tenant;
    return payload + "." + signature(payload);
}

AuthResult AuthToken::verify(
    std::string_view token,
    std::chrono::system_clock::time_point now) const
{
    if (!enabled() || token.empty())
        return {{}, AuthError::Missing};
    const auto parts = split(token, '.');
    if (parts.size() != 5 || parts[0] != "v1")
        return {{}, AuthError::Malformed};

    AuthIdentity identity;
    if (!parseUnsigned(parts[1], identity.userId) ||
        !parseUnsigned(parts[2], identity.expiresAtUnixSeconds) ||
        !validTenant(parts[3]) || parts[4].size() != 64)
        return {{}, AuthError::InvalidClaims};
    identity.tenant.assign(parts[3]);

    const auto payloadLength = token.size() - parts[4].size() - 1;
    const auto expected = signature(token.substr(0, payloadLength));
    if (expected.size() != parts[4].size() ||
        CRYPTO_memcmp(expected.data(), parts[4].data(), expected.size()) != 0)
        return {{}, AuthError::BadSignature};

    const auto nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    if (nowSeconds < 0 || identity.expiresAtUnixSeconds <=
                              static_cast<std::uint64_t>(nowSeconds))
        return {{}, AuthError::Expired};
    return {std::move(identity), AuthError::None};
}

std::optional<std::string> AuthToken::bearerToken(std::string_view authorization)
{
    authorization = trim(authorization);
    const auto separator = authorization.find(' ');
    if (separator == std::string_view::npos ||
        !iequals(authorization.substr(0, separator), "bearer"))
        return std::nullopt;
    const auto value = trim(authorization.substr(separator + 1));
    if (value.empty())
        return std::nullopt;
    return std::string(value);
}

std::optional<std::string> AuthToken::cookieToken(
    std::string_view cookieHeader,
    std::string_view cookieName)
{
    std::size_t start = 0;
    while (start <= cookieHeader.size())
    {
        const auto end = cookieHeader.find(';', start);
        auto part = trim(cookieHeader.substr(
            start, end == std::string_view::npos ? end : end - start));
        const auto equals = part.find('=');
        if (equals != std::string_view::npos &&
            trim(part.substr(0, equals)) == cookieName)
        {
            auto value = trim(part.substr(equals + 1));
            if (!value.empty())
                return std::string(value);
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return std::nullopt;
}

AuthResult AuthToken::authenticateHeaders(
    const std::unordered_map<std::string, std::string> &headers,
    std::chrono::system_clock::time_point now) const
{
    if (const auto found = headers.find("authorization");
        found != headers.end())
    {
        if (auto token = bearerToken(found->second))
            return verify(*token, now);
        return {{}, AuthError::Malformed};
    }
    if (const auto found = headers.find("cookie");
        found != headers.end())
    {
        if (auto token = cookieToken(found->second))
            return verify(*token, now);
    }
    return {{}, AuthError::Missing};
}

} // namespace webserver::security
