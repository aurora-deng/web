#include "server/phase11/security/SodiumCredentialCodec/SodiumCredentialCodec.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace webserver::phase11
{
namespace
{

constexpr std::string_view kTokenHashPrefix = "b2-v1:";

void validateConfig(const SodiumCredentialConfig &config)
{
    if (config.passwordOpsLimit < crypto_pwhash_OPSLIMIT_MIN ||
        config.passwordOpsLimit > crypto_pwhash_OPSLIMIT_MAX)
        throw std::invalid_argument("Argon2id ops limit is outside libsodium bounds");
    if (config.passwordMemLimit < crypto_pwhash_MEMLIMIT_MIN ||
        config.passwordMemLimit > crypto_pwhash_MEMLIMIT_MAX)
        throw std::invalid_argument("Argon2id memory limit is outside libsodium bounds");
    // 32 字节提供 256 位随机空间；上限避免异常配置制造过大的 Cookie/数据库字段。
    if (config.randomTokenBytes < 32 || config.randomTokenBytes > 64)
        throw std::invalid_argument("random token size must be between 32 and 64 bytes");
}

} // namespace

SodiumCredentialCodec::SodiumCredentialCodec(
    std::string_view tokenHashKeyHex, SodiumCredentialConfig config)
    : config_(config)
{
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium initialization failed");
    validateConfig(config_);

    std::size_t decodedLength = 0;
    const int decoded = sodium_hex2bin(
        tokenHashKey_.data(), tokenHashKey_.size(),
        tokenHashKeyHex.data(), tokenHashKeyHex.size(), nullptr,
        &decodedLength, nullptr);
    if (decoded != 0 || decodedLength != tokenHashKey_.size() ||
        tokenHashKeyHex.size() != tokenHashKey_.size() * 2)
    {
        sodium_memzero(tokenHashKey_.data(), tokenHashKey_.size());
        throw std::invalid_argument(
            "token hash key must contain exactly 64 hexadecimal characters");
    }

    // 不存在用户也必须执行同类型、同参数的 Argon2id 校验。随机 dummy 密码只在构造时
    // 使用，生成散列后立即清零，运行期不会保存一份可读的固定假密码。
    std::array<unsigned char, 32> dummyPassword{};
    randombytes_buf(dummyPassword.data(), dummyPassword.size());
    try
    {
        dummyPasswordHash_ = hashPassword(std::string_view{
            reinterpret_cast<const char *>(dummyPassword.data()),
            dummyPassword.size()});
    }
    catch (...)
    {
        sodium_memzero(dummyPassword.data(), dummyPassword.size());
        sodium_memzero(tokenHashKey_.data(), tokenHashKey_.size());
        throw;
    }
    sodium_memzero(dummyPassword.data(), dummyPassword.size());
}

SodiumCredentialCodec::~SodiumCredentialCodec()
{
    sodium_memzero(tokenHashKey_.data(), tokenHashKey_.size());
    if (!dummyPasswordHash_.empty())
        sodium_memzero(dummyPasswordHash_.data(), dummyPasswordHash_.size());
}

std::string SodiumCredentialCodec::hashPassword(
    std::string_view password) const
{
    if (password.size() > std::numeric_limits<unsigned long long>::max())
        throw std::length_error("password is too large for libsodium");

    std::array<char, crypto_pwhash_STRBYTES> encoded{};
    if (crypto_pwhash_str_alg(
            encoded.data(), password.data(),
            static_cast<unsigned long long>(password.size()),
            config_.passwordOpsLimit, config_.passwordMemLimit,
            crypto_pwhash_ALG_ARGON2ID13) != 0)
        throw std::runtime_error("Argon2id password hashing failed");
    return encoded.data();
}

std::string SodiumCredentialCodec::dummyPasswordHash() const
{
    return dummyPasswordHash_;
}

bool SodiumCredentialCodec::verifyPassword(
    std::string_view encoded, std::string_view password) const
{
    if (encoded.empty() || encoded.size() >= crypto_pwhash_STRBYTES)
        return false;
    std::string nullTerminated(encoded);
    return crypto_pwhash_str_verify(
               nullTerminated.c_str(), password.data(),
               static_cast<unsigned long long>(password.size())) == 0;
}

std::string SodiumCredentialCodec::randomToken() const
{
    std::array<unsigned char, 64> bytes{};
    randombytes_buf(bytes.data(), config_.randomTokenBytes);

    // sodium_base64_ENCODED_LEN 包含末尾 NUL。URLSAFE_NO_PADDING 适合 Cookie 和 JSON，
    // 不会出现需要额外转义的 '+'、'/'、'='。
    std::array<char,
               sodium_base64_ENCODED_LEN(64,
                   sodium_base64_VARIANT_URLSAFE_NO_PADDING)> encoded{};
    sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(),
                      config_.randomTokenBytes,
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    sodium_memzero(bytes.data(), bytes.size());
    return encoded.data();
}

std::string SodiumCredentialCodec::hashToken(std::string_view token) const
{
    std::array<unsigned char, kTokenHashBytes> digest{};
    if (crypto_generichash(
            digest.data(), digest.size(),
            reinterpret_cast<const unsigned char *>(token.data()), token.size(),
            tokenHashKey_.data(), tokenHashKey_.size()) != 0)
        throw std::runtime_error("keyed token hash failed");

    std::array<char, kTokenHashBytes * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
    sodium_memzero(digest.data(), digest.size());
    return std::string{kTokenHashPrefix} + hex.data();
}

bool SodiumCredentialCodec::verifyToken(
    std::string_view encoded, std::string_view token) const
{
    const auto expected = hashToken(token);
    if (encoded.size() != expected.size())
        return false;
    return sodium_memcmp(encoded.data(), expected.data(), expected.size()) == 0;
}

} // namespace webserver::phase11
