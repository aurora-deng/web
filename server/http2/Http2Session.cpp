#include "server/http2/Http2Session.h"

#include "server/CoroutineScheduler/AWaiter.h"
#include "server/Executor/Executor.h"
#include "server/Route/Router.h"
#include "server/SubReactor/SubReactor.h"
#include "server/transport/OutboundTask.h"

#include <algorithm>
#include <string>

namespace
{
constexpr size_t kMaxResponseBody = 1024 * 1024;

bool collectBody(const RespBodyPtr &source, std::string &bytes)
{
    if (!source)
        return true;
    if (source->Chunked() || source->useSendfile() || source->remain() > kMaxResponseBody)
        return false;
    while (!source->finished())
    {
        Block block{};
        const auto count = source->buildSegments(&block, 64 * 1024);
        if (count <= 0)
            return false;
        size_t copied = 0;
        for (int i = 0; i < block.idx; ++i)
        {
            const auto &segment = block.segs[i];
            if (segment.len > kMaxResponseBody - bytes.size())
                return false;
            bytes.append(segment.data, segment.len);
            copied += segment.len;
        }
        if (copied == 0)
            return false;
        source->consume(copied);
    }
    return true;
}
} // namespace

Http2Session::Http2Session(ConnectionKey key, SubReactor *reactor)
    : key_(key), reactor_(reactor)
{
}

Http2Session::Job::~Job()
{
    if (context.response)
        responsePool.release(context.response);
}

void Http2Session::requestHandlerStop() noexcept
{
    for (auto &[_, job] : jobs_)
        job->stop.request_stop();
}

void Http2Session::workerDone(const std::shared_ptr<Job> &job)
{
    {
        std::lock_guard<std::mutex> lock(completedMutex_);
        completed_.push_back(job);
    }
    reactor_->notifyExecuteComplete(key_.fd, key_.connId);
}

bool Http2Session::dispatchReady()
{
    for (auto &ready : codec_.takeReady())
    {
        auto job = std::make_shared<Job>();
        job->streamId = ready.streamId;
        auto &request = job->context.request;
        request.method = std::move(ready.method);
        request.raw_path = std::move(ready.rawPath);
        request.headers = std::move(ready.headers);
        request.bodyData = std::move(ready.body);
        request.bodySize = request.bodyData.size();
        request.version = "HTTP/2";
        const auto question = request.raw_path.find('?');
        request.path = request.raw_path.substr(0, question);
        if (question != std::string::npos)
        {
            request.query = request.raw_path.substr(question + 1);
            size_t pos = 0;
            while (pos < request.query.size())
            {
                const auto end = request.query.find('&', pos);
                const auto part = request.query.substr(
                    pos, end == std::string::npos ? end : end - pos);
                const auto equals = part.find('=');
                if (!part.empty())
                    request.querryParams[part.substr(0, equals)] =
                        equals == std::string::npos ? "" : part.substr(equals + 1);
                if (end == std::string::npos)
                    break;
                pos = end + 1;
            }
        }
        job->context.fd = key_.fd;
        job->context.Id = key_.connId;
        job->context.cancellation = HandlerCancellation{
            job->stop.get_token(), reactor_->executor().handlerDeadline()};
        jobs_[job->streamId] = job;
        auto self = shared_from_this();
        if (!reactor_->executor().submit([self, job]()
                                         {
            auto &ctx = job->context;
            try
            {
                if (!ctx.stopRequested())
                {
                    ctx.response = responsePool.acquire();
                    self->reactor_->router().handle(ctx);
                }
                if (ctx.handlerDeadlineExceeded())
                {
                    if (ctx.response)
                        responsePool.release(ctx.response);
                    ctx.response = responsePool.acquire();
                    ctx.response->status = 504;
                    ctx.response->text("Handler Timeout");
                }
            }
            catch (...)
            {
                if (ctx.response)
                    responsePool.release(ctx.response);
                ctx.response = responsePool.acquire();
                ctx.response->status = 500;
                ctx.response->text("Internal Server Error");
            }
            self->workerDone(job);
        }))
        {
            jobs_.erase(job->streamId);
            if (!codec_.submitResponse(job->streamId, 503, {}, "Service Unavailable"))
                return false;
        }
    }
    return true;
}

void Http2Session::reapClosed()
{
    for (const auto streamId : codec_.takeClosed())
    {
        auto it = jobs_.find(streamId);
        if (it != jobs_.end())
        {
            it->second->stop.request_stop();
            jobs_.erase(it);
        }
    }
}

bool Http2Session::finishCompleted()
{
    std::deque<std::shared_ptr<Job>> finished;
    {
        std::lock_guard<std::mutex> lock(completedMutex_);
        finished.swap(completed_);
    }
    for (auto &job : finished)
    {
        auto it = jobs_.find(job->streamId);
        if (it == jobs_.end() || it->second != job)
            continue; // RST_STREAM or connection close made this result stale.
        auto &ctx = job->context;
        int status = 200;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
        if (ctx.webSocketAccepted || ctx.sseAccepted)
        {
            status = 501; // Existing WS/SSE sessions own a whole HTTP/1.1 connection.
            body = "This route requires HTTP/1.1";
        }
        else if (ctx.response)
        {
            status = ctx.response->status;
            headers = ctx.response->headers;
            if (!collectBody(ctx.response->body, body))
            {
                status = 501;
                headers.clear();
                body = "Response body type is not supported over HTTP/2";
            }
        }
        else
        {
            status = 500;
            body = "Missing response";
        }
        if (!codec_.submitResponse(job->streamId, status, headers, std::move(body)))
            return false;
        jobs_.erase(it);
    }
    return true;
}

bool Http2Session::flushOutput()
{
    std::string bytes;
    while (true)
    {
        if (!codec_.drainOutput(bytes))
            return false;
        if (bytes.empty())
            return true;
        if (reactor_->enqueueOutbound(key_.fd,
                OutboundTask::encoded(std::move(bytes))) != EnqueueResult::Ok)
            return false;
        bytes.clear();
    }
}

Task<void> Http2Session::run()
{
    if (!codec_.valid())
    {
        reactor_->fd_close(key_.fd, "http2 codec initialization failed", CoroutineRole::Main);
        co_return;
    }
    while (true)
    {
        auto *conn = reactor_->findConnection(key_.fd, key_.connId);
        if (!conn)
            co_return;

        if (conn->readBuffer.readableBytes())
        {
            const auto length = conn->readBuffer.readableBytes();
            if (!codec_.receive(conn->readBuffer.peek(), length))
            {
                reactor_->fd_close(key_.fd, "invalid HTTP/2 frames", CoroutineRole::Main);
                co_return;
            }
            conn->readBuffer.retrieve(length);
            conn->pendingBytes = 0;
            conn->state.pauseByMemory = false;
        }
        reapClosed();
        if (!dispatchReady() || !finishCompleted() || !flushOutput())
        {
            reactor_->fd_close(key_.fd, "HTTP/2 stream or outbound failure", CoroutineRole::Main);
            co_return;
        }
        if (conn->state.peerClosed)
        {
            reactor_->fd_close(key_.fd, "HTTP/2 peer closed", CoroutineRole::Main);
            co_return;
        }
        const auto recvState = reactor_->recvSocket(key_.fd);
        if (recvState == RecvState::CLOSED)
            co_return;
        conn = reactor_->findConnection(key_.fd, key_.connId);
        if (!conn)
            co_return;
        if (conn->readBuffer.readableBytes())
            continue;
        co_await ReadAwaiter(reactor_, key_);
    }
}
