#pragma once

#include "server/http2/Http2Codec.h"
#include "server/http/RequestContext/RequestContext.h"
#include "server/session/Session/Session.h"
#include "server/transport/ConnectionKey.h"

#include <deque>
#include <memory>
#include <mutex>
#include <stop_token>
#include <unordered_map>

class SubReactor;

// A native h2c prior-knowledge connection. One coroutine owns the connection;
// many streams can be in the HTTP Executor concurrently.
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
        ~Job();
    };

    bool dispatchReady();
    bool finishCompleted();
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
