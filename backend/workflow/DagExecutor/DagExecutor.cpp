#include "DagExecutor.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <utility>

namespace nebula::workflow {
namespace {
struct Completion {
    WorkResult result;
    Clock::time_point finished;
};

struct Wakeup {
    std::mutex mutex;

    void notify() {
        if (!pending_.exchange(true, std::memory_order_acq_rel)) signal_.release();
    }

    void waitUntil(Clock::time_point deadline) {
        if (deadline == Clock::time_point::max()) signal_.acquire();
        else if (!signal_.try_acquire_until(deadline)) return;
        pending_.store(false, std::memory_order_release);
        // The scheduler inspects every mailbox after clearing the signal. A
        // producer that skipped a duplicate release is included in that scan.
    }

private:
    std::binary_semaphore signal_{0};
    std::atomic_bool pending_{false};
};

struct Attempt {
    std::stop_source stop;
    Clock::time_point start;
    Clock::time_point deadline;
    std::optional<Completion> completion;
};

struct Slot {
    NodeResult result;
    Clock::time_point readyAt{};
    std::shared_ptr<Attempt> running;
};

bool terminal(State state) {
    return state == State::success || state == State::failed || state == State::timeout ||
           state == State::skipped || state == State::cancelled || state == State::uncertain;
}

Clock::time_point clockNow(const std::function<Clock::time_point()>& clock) {
    return clock ? clock() : Clock::now();
}

WorkResult execute(const Node& node, const NodeContext& context, const std::function<Clock::time_point()>& clock) {
    if (context.stop.stop_requested() || clockNow(clock) >= context.deadline) {
        return {WorkStatus::failure, {}, "stopped"};
    }
    try {
        auto result = node.execute(context);
        if (result.value.size() > 4096) return {WorkStatus::failure, {}, "output_too_large"};
        if (!result.errorCode.empty() && !validIdentifier(result.errorCode)) {
            result.errorCode = "execution_failed";
        }
        return result;
    } catch (...) {
        return {WorkStatus::failure, {}, "execution_exception"};
    }
}

class Run {
public:
    Run(const WorkflowGraph& graph, RunOptions options, WorkerPool& workers)
        : graph_(graph), options_(std::move(options)), workers_(workers),
          deadline_(clockNow(options_.now) + options_.timeout), signals_(std::make_shared<Wakeup>()) {
        slots_.resize(graph_.steps().size());
        for (std::size_t i = 0; i < slots_.size(); ++i) slots_[i].result.nodeId = graph_.steps()[i].node.id;
    }

    ~Run() {
        // If scheduling throws, finish all jobs while their graph still lives.
        for (const auto& slot : slots_) if (slot.running) slot.running->stop.request_stop();
        for (;;) {
            {
                std::lock_guard lock(signals_->mutex);
                if (std::ranges::all_of(slots_, [](const auto& slot) {
                    return !slot.running || slot.running->completion.has_value();
                })) return;
            }
            signals_->waitUntil(Clock::time_point::max());
        }
    }

    RunResult start() {
        std::stop_callback cancel(options_.stop, [wakeup = signals_] { wakeup->notify(); });
        emit(EventKind::run, State::started);
        for (const auto& step : graph_.steps()) {
            for (const auto& id : step.node.needs) emit(EventKind::edge, State::pending, step.node.id, id);
        }

        for (;;) {
            const auto now = clockNow(options_.now);
            if (stop_ == State::pending) {
                if (options_.stop.stop_requested()) stop_ = State::cancelled;
                else if (now >= deadline_) stop_ = State::timeout;
            }
            auto nextWake = stop_ == State::pending ? deadline_ : Clock::time_point::max();
            for (std::size_t i = 0; i < slots_.size(); ++i) observe(i, now, nextWake);
            if (stop_ == State::pending) dispatch(now, nextWake);
            if (!active_ && std::ranges::all_of(slots_, [](const Slot& slot) {
                return terminal(slot.result.state);
            })) break;

            if (options_.now && nextWake != Clock::time_point::max()) {
                const auto remaining = std::clamp(nextWake - clockNow(options_.now), Clock::duration::zero(),
                                                  Clock::duration(Milliseconds{2}));
                signals_->waitUntil(Clock::now() + remaining);
            } else {
                signals_->waitUntil(nextWake);
            }
        }

        RunResult result{outcome(), options_.traceId, {}, peakActive_, peakReady_};
        for (auto& slot : slots_) result.nodes.push_back(std::move(slot.result));
        emit(EventKind::run, result.state);
        return result;
    }

private:
    void emit(EventKind kind, State state, std::string_view nodeId = {}, std::string_view predecessor = {},
              unsigned attempt = 0, double elapsed = 0.0, std::size_t count = 0, std::string_view error = {}) {
        if (!options_.onEvent) return;
        const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        lastNs_ = std::max(lastNs_ + 1, static_cast<std::uint64_t>(wall));
        try {
            options_.onEvent({kind, state, graph_.name(), options_.runId, options_.traceId, nodeId,
                              predecessor, attempt, lastNs_, ++sequence_, elapsed, count, error});
        } catch (...) {
            // Failure to export telemetry must not change a workflow outcome.
        }
    }

    void transition(std::size_t index, State state, std::string error = {}, double elapsed = 0) {
        auto& result = slots_[index].result;
        result.state = state;
        result.errorCode = std::move(error);
        emit(EventKind::node, state, result.nodeId, {}, std::max(1U, result.attempts), elapsed,
             graph_.steps()[index].dependencies.size(), result.errorCode);
    }

    void observe(std::size_t index, Clock::time_point now, Clock::time_point& nextWake) {
        auto& slot = slots_[index];
        auto attempt = slot.running;
        if (attempt) {
            std::optional<Completion> completion;
            {
                std::lock_guard lock(signals_->mutex);
                if (attempt->completion) completion = std::move(attempt->completion);
            }
            if (!terminal(slot.result.state)) {
                const bool external = graph_.steps()[index].node.effect == Effect::external;
                if (stop_ == State::cancelled) {
                    if (completion && external) finish(index, *attempt, std::move(*completion));
                    else transition(index, external ? State::uncertain : State::cancelled,
                                    external ? "outcome_uncertain" : "run_cancelled");
                } else if (completion) {
                    finish(index, *attempt, std::move(*completion));
                } else if (now >= attempt->deadline) {
                    transition(index, external ? State::uncertain : State::timeout,
                               external ? "outcome_uncertain" : "node_timeout",
                               std::chrono::duration<double, std::milli>(now - attempt->start).count());
                } else {
                    nextWake = std::min(nextWake, attempt->deadline);
                }
                if (terminal(slot.result.state) && !completion) attempt->stop.request_stop();
            }
            if (completion) {
                slot.running.reset();
                --active_;
            }
        } else if (!terminal(slot.result.state) && stop_ != State::pending) {
            transition(index, stop_, stop_ == State::timeout ? "run_timeout" : "run_cancelled");
        }
    }

    void finish(std::size_t index, const Attempt& attempt, Completion completed) {
        auto& slot = slots_[index];
        auto& work = completed.result;
        const auto elapsed = std::chrono::duration<double, std::milli>(completed.finished - attempt.start).count();
        if (work.status == WorkStatus::uncertain) {
            transition(index, State::uncertain,
                       work.errorCode.empty() ? "outcome_uncertain" : std::move(work.errorCode), elapsed);
        } else if (completed.finished >= attempt.deadline) {
            const bool external = graph_.steps()[index].node.effect == Effect::external;
            transition(index, external ? State::uncertain : State::timeout,
                       external ? "outcome_uncertain" : "node_timeout", elapsed);
        } else if (work.status == WorkStatus::success) {
            slot.result.value = std::move(work.value);
            transition(index, State::success, {}, elapsed);
        } else if (work.status == WorkStatus::retryable && stop_ == State::pending &&
                   slot.result.attempts < graph_.steps()[index].node.attempts) {
            slot.readyAt = completed.finished + graph_.steps()[index].node.backoff * slot.result.attempts;
            transition(index, State::retry, "transient", elapsed);
        } else {
            if (work.errorCode.empty()) work.errorCode = "execution_failed";
            transition(index, State::failed, std::move(work.errorCode), elapsed);
        }
    }

    void dispatch(Clock::time_point now, Clock::time_point& nextWake) {
        std::size_t ready = 0;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            auto& slot = slots_[i];
            const auto state = slot.result.state;
            if (state == State::pending) {
                const auto& deps = graph_.steps()[i].dependencies;
                const bool partial = graph_.steps()[i].node.acceptIncomplete;
                if (!partial && std::ranges::any_of(deps, [&](auto d) {
                    const auto parent = slots_[d].result.state;
                    return terminal(parent) && parent != State::success;
                })) {
                    transition(i, State::skipped, "dependency_failed");
                } else if (std::ranges::all_of(deps, [&](auto d) {
                    const auto state = slots_[d].result.state;
                    return partial ? terminal(state) : state == State::success;
                })) {
                    queue(i);
                }
            } else if (state == State::retry) {
                if (now >= slot.readyAt) queue(i);
                else nextWake = std::min(nextWake, slot.readyAt);
            }
            if (slot.result.state == State::queued) ++ready;
        }
        peakReady_ = std::max(peakReady_, ready);

        for (std::size_t i = 0; i < slots_.size() && active_ < workers_.capacity(); ++i) {
            const auto current = clockNow(options_.now);
            if (options_.stop.stop_requested() || current >= deadline_) {
                nextWake = std::min(nextWake, current);
                return;
            }
            if (slots_[i].result.state == State::queued) {
                launch(i);
                nextWake = std::min(nextWake, slots_[i].running->deadline);
            }
        }
    }

    void queue(std::size_t index) {
        ++slots_[index].result.attempts;
        transition(index, State::queued);
    }

    void launch(std::size_t index) {
        auto& slot = slots_[index];
        const auto& step = graph_.steps()[index];
        auto attempt = std::make_shared<Attempt>();
        attempt->start = clockNow(options_.now);
        attempt->deadline = std::min(deadline_, attempt->start + step.node.timeout);
        std::vector<DependencyValue> inputs;
        for (const auto dep : step.dependencies) {
            const auto& parent = slots_[dep].result;
            inputs.push_back({parent.nodeId, parent.value, parent.state});
        }
        auto key = options_.runId + ":" + step.node.id;
        transition(index, State::started);
        workers_.submit([node = &step.node, attempt, wakeup = signals_, runId = options_.runId,
                         trace = options_.traceId, key = std::move(key), number = slot.result.attempts, clock = options_.now,
                         inputs = std::move(inputs)] {
            const NodeContext context{runId, trace, key, number, attempt->deadline, attempt->stop.get_token(), inputs};
            auto result = execute(*node, context, clock);
            const auto finished = clockNow(clock);
            {
                std::lock_guard lock(wakeup->mutex);
                attempt->completion.emplace(Completion{std::move(result), finished});
            }
            wakeup->notify();
        });
        slot.running = std::move(attempt);
        peakActive_ = std::max(peakActive_, ++active_);
    }

    State outcome() const {
        if (std::ranges::any_of(slots_, [](const auto& slot) { return slot.result.state == State::uncertain; })) {
            return State::uncertain;
        }
        if (stop_ != State::pending) return stop_;
        if (std::ranges::any_of(slots_, [](const auto& slot) { return slot.result.state == State::timeout; })) return State::timeout;
        if (std::ranges::any_of(slots_, [](const auto& slot) { return slot.result.state == State::failed; })) return State::failed;
        return State::success;
    }

    const WorkflowGraph& graph_;
    RunOptions options_;
    WorkerPool& workers_;
    Clock::time_point deadline_;
    std::shared_ptr<Wakeup> signals_;
    std::vector<Slot> slots_;
    std::size_t active_{};
    std::size_t peakActive_{};
    std::size_t peakReady_{};
    State stop_{State::pending};
    std::uint64_t lastNs_{};
    std::uint64_t sequence_{};
};
}

std::string_view stateName(State state) {
    constexpr std::string_view names[]{"pending", "queued", "started", "success", "failed",
                                       "timeout", "retry", "skipped", "cancelled", "uncertain"};
    return names[static_cast<std::size_t>(state)];
}

DagExecutor::DagExecutor(std::size_t workers) : workers_(workers) {}

RunResult DagExecutor::run(const WorkflowGraph& graph, RunOptions options) {
    std::unique_lock lock(admission_, std::try_to_lock);
    if (!lock.owns_lock()) throw std::runtime_error("workflow executor is busy");
    if (!validIdentifier(options.runId, 64) || options.timeout < Milliseconds{1} ||
        options.timeout > Milliseconds{600000}) throw std::invalid_argument("invalid workflow run");
    if (options.traceId.empty()) options.traceId = newTraceId();
    if (!validTraceId(options.traceId)) throw std::invalid_argument("invalid workflow trace ID");
    return Run(graph, std::move(options), workers_).start();
}
}
