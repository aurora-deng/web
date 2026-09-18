#include "../../CoroutineScheduler/Task.h"

#include <cassert>
#include <coroutine>
#include <iostream>
#include <optional>
#include <vector>

// 这是业务流的协程演示，不解析 HTTP/2 帧；协议帧仍由 nghttp2 处理。
// 每次调用创建一个独立协程帧，第一次 resume 模拟提交 Worker 后挂起。
Task<int> handleStream(int streamId, std::vector<int> &events)
{
    events.push_back(streamId); // 已接单；此时其他 stream 可以继续运行。
    co_await std::suspend_always{};
    events.push_back(-streamId); // Worker 完成后，由 Reactor 恢复这条流。
    co_return streamId * 10;
}

int main()
{
    std::vector<int> events;
    std::optional<Task<int>> stream1{handleStream(1, events)};
    std::optional<Task<int>> stream3{handleStream(3, events)};
    std::optional<Task<int>> stream5{handleStream(5, events)};

    stream1->resume();
    stream3->resume();
    stream5->resume();
    assert(!stream1->done() && !stream3->done() && !stream5->done());

    // 模拟 stream 5 收到 RST_STREAM：销毁其挂起的协程，不再发送响应。
    stream5.reset();
    // 模拟 Worker 先完成 stream 3；完成次序不受 stream ID 限制。
    stream3->resume();
    assert(stream3->done() && stream3->result() == 30);
    stream1->resume();
    assert(stream1->done() && stream1->result() == 10);
    assert((events == std::vector<int>{1, 3, 5, -3, -1}));

    std::cout << "stream 1/3/5 started; stream 5 cancelled; "
                 "stream 3 completed before stream 1\n"
                 "PASS: independent stream coroutine lifecycle\n";
}
