#include "server/http2/Http2StreamCoroutine.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void check(bool condition, const char *expression)
{
    if (!condition)
        throw std::runtime_error(expression);
}
#define CHECK(expression) check((expression), #expression)

struct FrameProbe
{
    int *destroyed = nullptr;
    ~FrameProbe() { ++*destroyed; }
};

Task<bool> waitForWorker(int streamId, bool result,
                         std::vector<int> &events, int &destroyed)
{
    FrameProbe probe{&destroyed};
    events.push_back(streamId);
    co_await std::suspend_always{};
    events.push_back(-streamId);
    co_return result;
}

Task<bool> finishImmediately(bool result)
{
    co_return result;
}
} // namespace

int main()
{
    using State = Http2StreamCoroutine::State;
    std::vector<int> events;
    int destroyed = 0;
    Http2StreamCoroutine stream1;
    Http2StreamCoroutine stream3;
    Http2StreamCoroutine stream5;

    CHECK(stream1.start(waitForWorker(1, true, events, destroyed)));
    CHECK(stream3.start(waitForWorker(3, true, events, destroyed)));
    CHECK(stream5.start(waitForWorker(5, true, events, destroyed)));
    CHECK(stream1.resume() == State::Suspended);
    CHECK(stream3.resume() == State::Suspended);
    CHECK(stream5.resume() == State::Suspended);

    // RST_STREAM：取消只销毁 stream 5 的协程帧，不影响同连接的 1 和 3。
    stream5.cancel();
    CHECK(stream5.state() == State::Cancelled);
    CHECK(destroyed == 1);

    // HTTP/2 允许响应逆序完成：3 可以先于 1 被 Reactor 恢复。
    CHECK(stream3.resume() == State::Succeeded);
    CHECK(stream1.resume() == State::Succeeded);
    CHECK((events == std::vector<int>{1, 3, 5, -3, -1}));
    CHECK(destroyed == 3);

    Http2StreamCoroutine submitFailure;
    CHECK(submitFailure.start(waitForWorker(7, false, events, destroyed)));
    CHECK(submitFailure.resume() == State::Suspended);
    CHECK(submitFailure.resume() == State::Failed);
    CHECK(destroyed == 4);

    // Executor 拒绝等同步路径可在首次 resume 就结束，不应误判为挂起。
    Http2StreamCoroutine immediate;
    CHECK(immediate.start(finishImmediately(true)));
    CHECK(immediate.resume() == State::Succeeded);
    CHECK(!immediate.active());
    CHECK(!immediate.start(finishImmediately(true)));

    std::cout << "HTTP/2 stream coroutine: suspend, reverse completion, "
                 "failure and RST cancellation passed\n";
}
