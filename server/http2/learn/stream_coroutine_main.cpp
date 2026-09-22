#include "../Http2StreamCoroutine.h"

#include <cassert>
#include <coroutine>
#include <iostream>
#include <vector>

// 这是业务流的协程演示，不解析 HTTP/2 帧；协议帧仍由 nghttp2 处理。
// 每次调用创建一个独立协程帧，第一次 resume 模拟提交 Worker 后挂起。
Task<bool> handleStream(int streamId, std::vector<int> &events)
{
    events.push_back(streamId); // 已接单；此时其他 stream 可以继续运行。
    co_await std::suspend_always{};
    events.push_back(-streamId); // Worker 完成后，由 Reactor 恢复这条流。
    co_return true;
}

int main()
{
    std::vector<int> events;
    Http2StreamCoroutine stream1;
    Http2StreamCoroutine stream3;
    Http2StreamCoroutine stream5;

    stream1.start(handleStream(1, events));
    stream3.start(handleStream(3, events));
    stream5.start(handleStream(5, events));
    const auto firstWait = stream1.resume();
    const auto thirdWait = stream3.resume();
    const auto fifthWait = stream5.resume();
    assert(firstWait == Http2StreamCoroutine::State::Suspended);
    assert(thirdWait == Http2StreamCoroutine::State::Suspended);
    assert(fifthWait == Http2StreamCoroutine::State::Suspended);

    // 模拟 stream 5 收到 RST_STREAM：销毁其挂起的协程，不再发送响应。
    stream5.cancel();
    // 模拟 Worker 先完成 stream 3；完成次序不受 stream ID 限制。
    const auto thirdDone = stream3.resume();
    const auto firstDone = stream1.resume();
    assert(thirdDone == Http2StreamCoroutine::State::Succeeded);
    assert(firstDone == Http2StreamCoroutine::State::Succeeded);
    assert((events == std::vector<int>{1, 3, 5, -3, -1}));

    std::cout << "stream 1/3/5 started; stream 5 cancelled; "
                 "stream 3 completed before stream 1\n"
                 "PASS: independent stream coroutine lifecycle\n";
}
