#include "FleetService.h"

#include "backend/workflow/WorkflowGraph/WorkflowGraph.h"

#include <thread>
#include <utility>

namespace nebula::fleet::wire {
using grpc::Status;
using grpc::StatusCode;
using namespace std::chrono_literals;

namespace {
std::string_view metadata(grpc::ServerContext& context, const char* key) {
    const auto [first, last] = context.client_metadata().equal_range(key);
    if (first == last || std::next(first) != last) return {};
    return {first->second.data(), first->second.size()};
}

Status failure(const Error& error) {
    switch (error.code()) {
    case ErrorCode::invalid: return {StatusCode::INVALID_ARGUMENT, error.what()};
    case ErrorCode::missing: return {StatusCode::NOT_FOUND, error.what()};
    case ErrorCode::conflict: return {StatusCode::FAILED_PRECONDITION, error.what()};
    case ErrorCode::exhausted: return {StatusCode::RESOURCE_EXHAUSTED, error.what()};
    case ErrorCode::offline: return {StatusCode::UNAVAILABLE, error.what()};
    case ErrorCode::storage: return {StatusCode::INTERNAL, "fleet ledger unavailable"};
    }
    return {StatusCode::INTERNAL, "fleet request failed"};
}

template<class Call>
Status handle(Call&& call) {
    try { return call(); }
    catch (const Error& error) { return failure(error); }
    catch (const std::exception&) { return {StatusCode::INTERNAL, "fleet request failed"}; }
}

struct Connected {
    FleetCoordinator& coordinator;
    Session session;
    ~Connected() { coordinator.disconnect(session); }
};

struct ActiveWatch {
    std::counting_semaphore<2>& slots;
    std::atomic_uint& current;
    ~ActiveWatch() { --current; slots.release(); }
};
}

FleetService::FleetService(FleetCoordinator& coordinator, std::string operatorToken, Robots robots, AgentCredentials agent)
    : coordinator_(coordinator), operatorToken_(std::move(operatorToken)), robots_(std::move(robots)),
      agent_(std::move(agent)) {}

grpc::Status FleetService::Submit(grpc::ServerContext* context, const v1::TaskRequest* in, v1::TaskStatus* out) {
    if (!tokenMatches(metadata(*context, tokenHeader), operatorToken_)) {
        return {StatusCode::UNAUTHENTICATED, "operator credential required"};
    }
    return handle([&] {
        encode(coordinator_.submit(in->task_id(), static_cast<Station>(in->station()), workflow::newTraceId()), *out);
        return grpc::Status::OK;
    });
}

grpc::Status FleetService::Status(grpc::ServerContext* context, const v1::TaskQuery* in, v1::TaskStatus* out) {
    if (!mayRead(*context)) {
        return {StatusCode::UNAUTHENTICATED, "status credential required"};
    }
    return handle([&] {
        encode(coordinator_.status(in->task_id()), *out);
        return grpc::Status::OK;
    });
}

grpc::Status FleetService::ListTasks(grpc::ServerContext* context, const v1::TaskPageRequest* in, v1::TaskPage* out) {
    if (!mayRead(*context)) return {StatusCode::UNAUTHENTICATED, "task list credential required"};
    if (in->limit() < 1 || in->limit() > 16 || in->before_epoch() < 0) {
        return {StatusCode::INVALID_ARGUMENT, "invalid task page cursor or limit"};
    }
    return handle([&] {
        auto rows = coordinator_.listBefore(in->before_epoch(), in->limit() + 1);
        if (rows.size() > in->limit()) {
            rows.pop_back();
            out->set_next_before_epoch(rows.back().command.epoch);
        }
        for (const auto& row : rows) encode(row, *out->add_tasks());
        return grpc::Status::OK;
    });
}

bool FleetService::mayRead(grpc::ServerContext& context) const {
    const auto credential = metadata(context, tokenHeader);
    return tokenMatches(credential, operatorToken_) || (!agent_.read.empty() && tokenMatches(credential, agent_.read));
}

void FleetService::sampleLoad(v1::LoadUpdate& out) {
    const auto load = coordinator_.load();
    out.set_queued(load.queued);
    out.set_active(load.active);
    out.set_connected(load.connected);
    out.set_queue_limit(load.queueLimit);
    out.set_watchers(watchers_.load());
    out.set_watch_rejections(watchRejections_.load());
}

grpc::Status FleetService::Load(grpc::ServerContext* context, const v1::LoadQuery*, v1::LoadUpdate* out) {
    if (!mayRead(*context)) return {StatusCode::UNAUTHENTICATED, "status credential required"};
    return handle([&] {
        sampleLoad(*out);
        return grpc::Status::OK;
    });
}

grpc::Status FleetService::WatchLoad(grpc::ServerContext* context,
                                    grpc::ServerReaderWriter<v1::LoadUpdate, v1::LoadWatch>* stream) {
    if (!mayRead(*context)) return {StatusCode::UNAUTHENTICATED, "status credential required"};
    if (context->deadline() > std::chrono::system_clock::now() + 5s) {
        return {StatusCode::INVALID_ARGUMENT, "load watch requires a short deadline"};
    }
    if (!watchSlots_.try_acquire()) {
        ++watchRejections_;
        return {StatusCode::RESOURCE_EXHAUSTED, "load watchers at capacity"};
    }
    ++watchers_;
    ActiveWatch watch{watchSlots_, watchers_};
    return handle([&] {
        using Clock = std::chrono::steady_clock;
        v1::LoadWatch credit;
        if (!stream->Read(&credit)) return grpc::Status::OK;
        if (credit.interval_ms() < 100 || credit.interval_ms() > 250) {
            return grpc::Status(StatusCode::INVALID_ARGUMENT, "load watch requires 100-250 ms samples");
        }
        const auto period = std::chrono::milliseconds{credit.interval_ms()};
        const auto started = Clock::now();
        unsigned last = 0;
        do {
            if (credit.interval_ms() != period.count()) {
                return grpc::Status(StatusCode::INVALID_ARGUMENT, "load watch interval changed");
            }
            while (!context->IsCancelled() && Clock::now() < started + period * last) {
                std::this_thread::sleep_until(started + period * last);
            }
            if (context->IsCancelled()) break;
            const auto sequence = static_cast<unsigned>((Clock::now() - started) / period) + 1;
            v1::LoadUpdate update;
            // Produce a current snapshot only for an application read credit.
            sampleLoad(update);
            update.set_sequence(sequence);
            update.set_skipped_samples(sequence - last - 1);
            if (!stream->Write(update)) break;
            last = sequence;
        } while (stream->Read(&credit));
        return grpc::Status::OK;
    });
}

grpc::Status FleetService::StageReview(grpc::ServerContext* context, const v1::ReviewDraft* in, v1::SimulationReview* out) {
    if (agent_.draft.empty()) return {StatusCode::UNIMPLEMENTED, "simulation reviews are disabled"};
    if (!tokenMatches(metadata(*context, tokenHeader), agent_.draft)) {
        return {StatusCode::UNAUTHENTICATED, "draft credential required"};
    }
    return handle([&] {
        encode(coordinator_.stageReview({in->trace_id(), in->source_task_id(), in->source_epoch()}), *out);
        return grpc::Status::OK;
    });
}

grpc::Status FleetService::InspectReview(grpc::ServerContext* context, const v1::ReviewQuery* in, v1::SimulationReview* out) {
    if (agent_.review.empty()) return {StatusCode::UNIMPLEMENTED, "simulation reviews are disabled"};
    if (!tokenMatches(metadata(*context, tokenHeader), agent_.review)) {
        return {StatusCode::UNAUTHENTICATED, "review credential required"};
    }
    return handle([&] {
        encode(coordinator_.inspectReview(in->trace_id()), *out);
        return grpc::Status::OK;
    });
}

grpc::Status FleetService::DecideReview(grpc::ServerContext* context, const v1::ReviewRequest* in, v1::SimulationReview* out) {
    if (agent_.review.empty()) return {StatusCode::UNIMPLEMENTED, "simulation reviews are disabled"};
    if (!tokenMatches(metadata(*context, tokenHeader), agent_.review)) {
        return {StatusCode::UNAUTHENTICATED, "review credential required"};
    }
    return handle([&] {
        encode(coordinator_.decideReview(in->trace_id(), static_cast<Decision>(in->decision())), *out);
        return grpc::Status::OK;
    });
}

grpc::Status FleetService::Exchange(grpc::ServerContext* context,
                                     grpc::ServerReaderWriter<v1::CoordinatorFrame, v1::EdgeFrame>* stream) {
    const auto id = metadata(*context, robotHeader);
    const auto robot = robots_.find(id);
    if (robot == robots_.end() || !tokenMatches(metadata(*context, tokenHeader), robot->second.token)) {
        return {StatusCode::UNAUTHENTICATED, "robot credential required"};
    }
    if (context->deadline() > std::chrono::system_clock::now() + 5s) {
        return {StatusCode::INVALID_ARGUMENT, "robot stream deadline is required"};
    }
    return handle([&]() -> grpc::Status {
        v1::EdgeFrame frame;
        if (!stream->Read(&frame) || !frame.has_hello() || frame.hello().version() != protocolVersion ||
            !validId(frame.hello().boot_id()) || !validCapabilities(frame.hello().stations()) ||
            (frame.hello().stations() & ~robot->second.allowed) != 0) {
            return {StatusCode::INVALID_ARGUMENT, "invalid robot handshake"};
        }
        if (!acceptsRobotSkills(frame.hello())) return {StatusCode::INVALID_ARGUMENT, "unsupported robot skills"};
        const auto attach = [&] {
            try { return coordinator_.connect(std::string(id), frame.hello().stations()); }
            catch (const Error& error) {
                if (error.code() == ErrorCode::conflict) {
                    throw Error(ErrorCode::offline, "previous stream still owns robot session");
                }
                throw;
            }
        };
        Connected connection{coordinator_, attach()};
        do {
            std::optional<Receipt> receipt;
            if (frame.has_receipt()) receipt = decode(frame.receipt());
            v1::CoordinatorFrame response;
            response.set_version(protocolVersion);
            const auto command = coordinator_.exchange(connection.session, receipt);
            if (receipt) encode(*receipt, *response.mutable_confirmed());
            if (command) encode(*command, *response.mutable_command());
            if (!stream->Write(response)) break;
            if (!stream->Read(&frame)) break;
            if (!frame.has_heartbeat() && !frame.has_receipt()) {
                return {StatusCode::INVALID_ARGUMENT, "unexpected frame after handshake"};
            }
        } while (!context->IsCancelled());
        return grpc::Status::OK;
    });
}
}
