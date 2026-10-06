#pragma once

#include "server/phase11/ports/Ports/Ports.h"

#include <sodium.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace webserver::phase11
{

struct SodiumCredentialConfig
{
    unsigned long long passwordOpsLimit = crypto_pwhash_OPSLIMIT_INTERACTIVE;
    std::size_t passwordMemLimit = crypto_pwhash_MEMLIMIT_INTERACTIVE;
    std::size_t randomTokenBytes = 32;
};

/**
 * Phase 11 的正式凭据实现。
 *
 * - 密码使用 libsodium Argon2id 字符串格式；随机盐、参数和算法版本都保存在结果中。
 * - 浏览器持有的 Session/CSRF Token 是 256 位随机值，并使用 URL-safe Base64 编码。
 * - 数据库只保存带服务器密钥的 BLAKE2b Token 指纹，不能从指纹还原 Token。
 *
 * tokenHashKeyHex 必须是 32 字节（64 个十六进制字符）的持久密钥。生产环境应从权限受限
 * 的密钥文件或 Secret Manager 注入；如果每次启动随机生成，旧 Session 会全部失效。
 */
class SodiumCredentialCodec final : public ICredentialCodec
{
public:
    explicit SodiumCredentialCodec(
        std::string_view tokenHashKeyHex,
        SodiumCredentialConfig config = {});
    ~SodiumCredentialCodec() override;

    SodiumCredentialCodec(const SodiumCredentialCodec &) = delete;
    SodiumCredentialCodec &operator=(const SodiumCredentialCodec &) = delete;
    SodiumCredentialCodec(SodiumCredentialCodec &&) = delete;
    SodiumCredentialCodec &operator=(SodiumCredentialCodec &&) = delete;

    [[nodiscard]] std::string hashPassword(
        std::string_view password) const override;
    [[nodiscard]] std::string dummyPasswordHash() const override;
    [[nodiscard]] bool verifyPassword(std::string_view encoded,
                                      std::string_view password) const override;
    [[nodiscard]] std::string randomToken() const override;
    [[nodiscard]] std::string hashToken(
        std::string_view token) const override;
    [[nodiscard]] bool verifyToken(std::string_view encoded,
                                   std::string_view token) const override;

private:
    static constexpr std::size_t kTokenHashBytes = 32;

    SodiumCredentialConfig config_;
    std::array<unsigned char, crypto_generichash_KEYBYTES> tokenHashKey_{};
    std::string dummyPasswordHash_;
};

} // namespace webserver::phase11
