#pragma once

#include "server/http2/Http2Codec.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/session/Session/Session.h"
#include "server/transport/ConnectionKey.h"

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>

class SubReactor;

// A native HTTP/2 connection: the root coroutine owns nghttp2 and socket I/O;
// each completed request has a stream coroutine that awaits its Executor job.
class Http2Session : public Session, public std::enable_shared_from_this<Http2Session>
{
public:
    Http2Session(ConnectionKey key, SubReactor *reactor);
    Task<void> run() override;
    void requestHandlerStop() noexcept override;
    bool wakeReadOnExecuteComplete() const override { return true; }

private:
    struct Job
    {
        int32_t streamId = 0;
        RequestContext context;
        std::stop_source stop;
        // Only the owning Reactor resumes this coroutine or calls nghttp2.
        // A cancelled Job may be destroyed after its Worker releases the last
        // shared_ptr; destroying a suspended Task never resumes it.
        std::optional<Task<bool>> coroutine;
        ~Job();
    };

    bool dispatchReady();
    bool finishCompleted();
    Task<bool> runStream(Job *job);
    bool flushOutput();
    void workerDone(const std::shared_ptr<Job> &job);
    void reapClosed();

    ConnectionKey key_;
    SubReactor *reactor_;
    Http2Codec codec_;
    std::unordered_map<int32_t, std::shared_ptr<Job>> jobs_;
    std::mutex completedMutex_;
    std::deque<std::shared_ptr<Job>> completed_;
};
