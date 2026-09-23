#include "server/security/AuthToken.h"
#include "server/security/FixedWindowRateLimiter.h"
#include "server/sse/SseReplayBuffer.h"

#include <cassert>
#include <chrono>
#include <string>

namespace
{
void verifyAuthenticationTokens()
{
    using namespace std::chrono;
    using webserver::security::AuthError;
    using webserver::security::AuthIdentity;
    using webserver::security::AuthToken;

    AuthToken tokens("phase9-test-secret-with-at-least-32-bytes");
    const auto now = system_clock::time_point{seconds{1'000}};
    const auto token = tokens.issue(AuthIdentity{42, "learning", 1'100});

    const auto accepted = tokens.verify(token, now);
    assert(accepted);
    assert(accepted.identity->userId == 42);
    assert(accepted.identity->tenant == "learning");

    auto tampered = token;
    tampered.back() = tampered.back() == '0' ? '1' : '0';
    assert(tokens.verify(tampered, now).error == AuthError::BadSignature);
    assert(tokens.verify(token, system_clock::time_point{seconds{1'100}}).error ==
           AuthError::Expired);

    const auto bearer = AuthToken::bearerToken("  Bearer   " + token + "  ");
    assert(bearer && *bearer == token);
    const auto cookie = AuthToken::cookieToken(
        "theme=dark; web_session=" + token + "; lang=zh-CN");
    assert(cookie && *cookie == token);
}

void verifyBoundedRateLimiter()
{
    using namespace std::chrono;
    webserver::security::FixedWindowRateLimiter limiter(
        2, milliseconds{100}, 2);
    const auto start = webserver::security::FixedWindowRateLimiter::TimePoint{};

    assert(limiter.allow(10, start));
    assert(limiter.allow(10, start));
    assert(!limiter.allow(10, start));
    assert(limiter.allow(10, start + milliseconds{100}));

    assert(limiter.allow(20, start + milliseconds{100}));
    // 身份表达到上限时拒绝新身份，防止随机 uid 撑爆限流器本身。
    assert(!limiter.allow(30, start + milliseconds{100}));
    // 两个窗口以后旧身份会被清理，新身份才能进入。
    assert(limiter.allow(30, start + milliseconds{301}));
}

void verifySseReplayWindow()
{
    SseReplayBuffer replay(/*eventsPerClient=*/2, /*maxClients=*/2);
    auto first = replay.append(7, SseEvent{"one", "notice", "1", {}});
    auto second = replay.append(7, SseEvent{"two", "notice", "2", {}});
    auto third = replay.append(7, SseEvent{"three", "notice", "3", {}});
    assert(first.id == "1" && second.id == "2" && third.id == "3");

    const auto afterSecond = replay.replayAfter(7, "2");
    assert(!afterSecond.gap && afterSecond.events.size() == 1);
    assert(afterSecond.events.front().data == "three");

    // 事件 1 已被大小为 2 的窗口淘汰，必须明确告诉客户端出现历史缺口。
    assert(replay.replayAfter(7, "1").gap);
    assert(replay.eventCount() == 2);

    replay.append(8, SseEvent{"eight", "message", "8", {}});
    replay.append(9, SseEvent{"nine", "message", "9", {}});
    // 最久未访问的 client=7 被淘汰，避免历史表无限增长。
    assert(replay.replayAfter(7, "2").gap);
}
} // namespace

int main()
{
    verifyAuthenticationTokens();
    verifyBoundedRateLimiter();
    verifySseReplayWindow();
    return 0;
}
