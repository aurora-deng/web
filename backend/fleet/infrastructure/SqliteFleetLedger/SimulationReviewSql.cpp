#include "SqliteFleetLedger.h"

#include <limits>

namespace nebula::fleet {
namespace {

bool currentSource(const std::optional<Task>& source, std::int64_t epoch) {
    return source && source->command.epoch == epoch && source->phase == Phase::queued && source->robotId.empty();
}

void checkReviewTime(std::int64_t now) {
    if (now <= 0 || now > std::numeric_limits<std::int64_t>::max() - reviewLifetimeSeconds) {
        throw Error(ErrorCode::invalid, "invalid review time");
    }
}

SimulationReview currentReview(SimulationReview review, std::int64_t now) {
    if (review.state == ReviewState::pending && (now < review.createdAt || now >= review.expiresAt)) {
        review.state = ReviewState::expired;
    }
    return review;
}

}

void SqliteFleetLedger::createReviewTable() {
    db_.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS agent_sim_reviews (
            trace_id TEXT PRIMARY KEY,
            source_task_id TEXT NOT NULL REFERENCES tasks(task_id),
            source_epoch INTEGER NOT NULL CHECK(source_epoch > 0),
            station INTEGER NOT NULL CHECK(station IN (1,2)),
            state INTEGER NOT NULL DEFAULT 1 CHECK(state BETWEEN 1 AND 5),
            created_at INTEGER NOT NULL,
            expires_at INTEGER NOT NULL CHECK(expires_at > created_at),
            replacement_task_id TEXT NOT NULL DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS agent_sim_reviews_pending ON agent_sim_reviews(expires_at) WHERE state=1;
    )SQL");
}

std::optional<SimulationReview> SqliteFleetLedger::lookupReview(std::string_view traceId) {
    if (!validTrace(traceId)) throw Error(ErrorCode::invalid, "invalid proposal ID");
    sqlite::Statement row(db_, R"SQL(
        SELECT source_task_id, source_epoch, station, state, created_at, expires_at, replacement_task_id
        FROM agent_sim_reviews WHERE trace_id=?
    )SQL");
    row.bind(1, traceId);
    if (!row.next()) return std::nullopt;
    return SimulationReview{{std::string(traceId), row.string(0), row.integer(1)}, static_cast<Station>(row.integer(2)),
                            static_cast<ReviewState>(row.integer(3)), row.integer(4), row.integer(5), row.string(6)};
}

SimulationReview SqliteFleetLedger::stageReview(const ReviewDraft& draft, std::int64_t now) {
    checkReviewTime(now);
    if (!validId(draft.sourceTaskId) || draft.sourceEpoch <= 0 || draft.sourceTaskId.starts_with(replacementPrefix)) {
        throw Error(ErrorCode::invalid, "invalid simulation proposal source");
    }
    sqlite::Transaction write(db_);
    if (const auto existing = lookupReview(draft.traceId)) {
        if (existing->draft != draft) throw Error(ErrorCode::conflict, "proposal ID already binds other evidence");
        write.commit();
        return currentReview(*existing, now);
    }
    const auto source = lookup(draft.sourceTaskId);
    if (!currentSource(source, draft.sourceEpoch)) throw Error(ErrorCode::conflict, "proposal evidence has changed");
    {
        sqlite::Statement capacity(db_, "SELECT 1 FROM agent_sim_reviews "
                                        "WHERE state=1 AND expires_at>? AND created_at<=? LIMIT 1 OFFSET 4095");
        capacity.bind(1, now).bind(2, now);
        if (capacity.next()) throw Error(ErrorCode::exhausted, "pending proposal limit reached");
    }
    const auto expiresAt = now + reviewLifetimeSeconds;
    sqlite::Statement(db_, R"SQL(
        INSERT INTO agent_sim_reviews(trace_id,source_task_id,source_epoch,station,created_at,expires_at)
        VALUES (?,?,?,?,?,?)
    )SQL").bind(1, draft.traceId).bind(2, draft.sourceTaskId).bind(3, draft.sourceEpoch)
          .bind(4, static_cast<int>(source->command.station)).bind(5, now).bind(6, expiresAt).run();
    write.commit();
    return {draft, source->command.station, ReviewState::pending, now, expiresAt, {}};
}

SimulationReview SqliteFleetLedger::inspectReview(std::string_view traceId, std::int64_t now) {
    checkReviewTime(now);
    const auto review = lookupReview(traceId);
    if (!review) throw Error(ErrorCode::missing, "unknown simulation proposal");
    return currentReview(*review, now);
}

SimulationReview SqliteFleetLedger::decideReview(std::string_view traceId, Decision decision, std::int64_t now) {
    checkReviewTime(now);
    if (decision != Decision::approve && decision != Decision::reject) {
        throw Error(ErrorCode::invalid, "invalid review decision");
    }
    sqlite::Transaction write(db_);
    const auto saved = lookupReview(traceId);
    if (!saved) throw Error(ErrorCode::missing, "unknown simulation proposal");
    if (saved->state != ReviewState::pending) {
        write.commit();
        return *saved;
    }
    auto review = currentReview(*saved, now);
    if (review.state == ReviewState::pending) {
        if (decision == Decision::reject) {
            review.state = ReviewState::rejected;
        } else {
            const auto source = lookup(review.draft.sourceTaskId);
            if (!currentSource(source, review.draft.sourceEpoch) || source->command.station != review.station) {
                review.state = ReviewState::stale;
            } else {
                review.replacementTaskId = std::string(replacementPrefix) + review.draft.traceId;
                if (lookup(review.replacementTaskId)) throw Error(ErrorCode::conflict, "replacement ID already exists");
                sqlite::Statement(db_, "UPDATE tasks SET phase=?,outcome=? WHERE epoch=?")
                    .bind(1, static_cast<int>(Phase::failed)).bind(2, supersededOutcome).bind(3, review.draft.sourceEpoch).run();
                insertTask(review.replacementTaskId, review.station, review.draft.traceId);
                review.state = ReviewState::approved;
            }
        }
    }
    sqlite::Statement(db_, "UPDATE agent_sim_reviews SET state=?,replacement_task_id=? WHERE trace_id=?")
        .bind(1, static_cast<int>(review.state)).bind(2, review.replacementTaskId).bind(3, traceId).run();
    write.commit();
    return review;
}

}
