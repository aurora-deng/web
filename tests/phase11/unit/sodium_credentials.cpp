#include "server/phase11/business/AuthService/AuthService.h"
#include "server/phase11/security/InMemoryLoginRateLimiter/InMemoryLoginRateLimiter.h"
#include "server/phase11/security/SodiumCredentialCodec/SodiumCredentialCodec.h"
#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

using namespace webserver::phase11;

namespace
{

constexpr std::string_view kTestTokenHashKey =
    "000102030405060708090a0b0c0d0e0f"
    "101112131415161718191a1b1c1d1e1f";
constexpr std::string_view kOtherTestTokenHashKey =
    "f0e0d0c0b0a090807060504030201000"
    "ffeeddccbbaa99887766554433221100";

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool isUrlSafeToken(std::string_view token)
{
    if (token.size() < 43)
        return false;
    for (const unsigned char value : token)
    {
        if (!((value >= 'a' && value <= 'z') ||
              (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_'))
            return false;
    }
    return true;
}

void verifyCodecPrimitives()
{
    SodiumCredentialCodec codec(kTestTokenHashKey);
    const auto firstPasswordHash = codec.hashPassword("correct horse battery");
    const auto secondPasswordHash = codec.hashPassword("correct horse battery");
    require(firstPasswordHash.starts_with("$argon2id$"),
            "password hash is not Argon2id");
    require(firstPasswordHash != secondPasswordHash,
            "password hashes reused the same salt");
    require(codec.verifyPassword(firstPasswordHash, "correct horse battery"),
            "correct password was rejected");
    require(!codec.verifyPassword(firstPasswordHash, "wrong password"),
            "wrong password was accepted");
    require(!codec.verifyPassword("not-a-password-hash", "password"),
            "malformed password hash was accepted");
    require(!codec.verifyPassword(codec.dummyPasswordHash(), "password"),
            "dummy password hash unexpectedly matched");

    std::unordered_set<std::string> tokens;
    for (int index = 0; index < 128; ++index)
    {
        auto token = codec.randomToken();
        require(isUrlSafeToken(token), "random token is not URL-safe");
        require(tokens.insert(std::move(token)).second,
                "random token collision occurred");
    }

    const auto token = codec.randomToken();
    const auto tokenHash = codec.hashToken(token);
    require(tokenHash.starts_with("b2-v1:"), "token hash version is missing");
    require(tokenHash == codec.hashToken(token),
            "token hash is not deterministic");
    require(codec.verifyToken(tokenHash, token), "correct token was rejected");
    require(!codec.verifyToken(tokenHash, token + "changed"),
            "wrong token was accepted");

    SodiumCredentialCodec sameKey(kTestTokenHashKey);
    SodiumCredentialCodec otherKey(kOtherTestTokenHashKey);
    require(sameKey.verifyToken(tokenHash, token),
            "persisted key could not verify an existing session");
    require(!otherKey.verifyToken(tokenHash, token),
            "different server key verified an existing session");
}

void verifyAuthFlow()
{
    InMemoryStore store;
    InMemoryLoginRateLimiter rateLimiter;
    SodiumCredentialCodec credentials(kTestTokenHashKey);
    AuthService auth(store, store, credentials, rateLimiter);
    const auto now = std::chrono::system_clock::now();

    const auto user = auth.registerUser(
        "Sodium_User", "strong-password", "Sodium User", now);
    const auto persisted = store.findUser(user.id);
    require(persisted && persisted->passwordHash.starts_with("$argon2id$"),
            "AuthService did not persist an Argon2id hash");
    require(persisted->passwordHash != "strong-password",
            "plaintext password was persisted");

    try
    {
        (void)auth.login("sodium_user", "wrong-password", now, "127.0.0.1");
        throw std::runtime_error("wrong password login unexpectedly succeeded");
    }
    catch (const ApplicationError &error)
    {
        require(error.code() == ErrorCode::Unauthenticated,
                "wrong password returned the wrong application error");
    }

    const auto login = auth.login(
        "sodium_user", "strong-password", now, "127.0.0.1");
    require(auth.authenticate(login.sessionToken, now).has_value(),
            "fresh session did not authenticate");
    require(auth.verifyCsrf(login.sessionToken, login.csrfToken, now),
            "correct CSRF token was rejected");
    require(!auth.verifyCsrf(login.sessionToken, "wrong-csrf", now),
            "wrong CSRF token was accepted");
    require(!store.findSessionByTokenHash(login.sessionToken).has_value(),
            "plaintext session token was stored as its own lookup key");
    require(!auth.authenticate(login.sessionToken, now + std::chrono::hours(25)),
            "expired session remained valid");
    auth.logout(login.sessionToken, now);
    require(!auth.authenticate(login.sessionToken, now),
            "revoked session remained valid");
}

} // namespace

int main()
{
    try
    {
        verifyCodecPrimitives();
        verifyAuthFlow();
        std::cout << "phase11 libsodium credential tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "phase11 libsodium credential tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
