#include "backend/application/observability/BusinessMetricsFormatter/BusinessMetricsFormatter.h"
#include <array>
#include <sstream>
namespace webserver::phase11 {
std::string formatBusinessMetrics(PostgresStore *store, PostgresStore *deliveryStore, DatabaseExecutor *executor, BoundedExecutor *fileExecutor, BoundedExecutor *knowledgeExecutor, transport::Phase11OutboxPump *pump, AiChatService *ai, const std::vector<std::shared_ptr<ResilientModelProvider>> &resilient) {

                const auto pool = store->poolSnapshot();
                const auto queue = executor->snapshot();
                const auto files = fileExecutor->snapshot();
                const auto knowledge = knowledgeExecutor->snapshot();
                const auto outbox = pump->snapshot();
                const auto aiChat = ai->snapshot();
                webserver::phase11::ModelProviderResilienceSnapshot aiProviders;
                for (const auto &provider : resilient)
                {
                    const auto current = provider->snapshot();
                    aiProviders.requests += current.requests;
                    aiProviders.primarySuccesses += current.primarySuccesses;
                    aiProviders.primaryFailures += current.primaryFailures;
                    aiProviders.fallbackAttempts += current.fallbackAttempts;
                    aiProviders.fallbackSuccesses += current.fallbackSuccesses;
                    aiProviders.fallbackFailures += current.fallbackFailures;
                    aiProviders.circuitOpened += current.circuitOpened;
                    aiProviders.circuitRejected += current.circuitRejected;
                    aiProviders.timedOut += current.timedOut;
                    aiProviders.active += current.active;
                }
                std::ostringstream text;
                if(deliveryStore){const auto delivery=deliveryStore->poolSnapshot();
                    text<<"webserver_function3_delivery_pool_in_use "<<delivery.inUse<<'\n'
                        <<"webserver_function3_delivery_pool_waited_total "<<delivery.waitedAcquires<<'\n'
                        <<"webserver_function3_delivery_pool_wait_microseconds_total "<<delivery.totalAcquireWaitMicroseconds<<'\n';}
                text << "# TYPE webserver_phase11_db_executor_submitted_total counter\n"
                     << "webserver_phase11_db_executor_submitted_total " << queue.submitted << '\n'
                     << "webserver_phase11_db_executor_rejected_total " << queue.rejected << '\n'
                     << "webserver_phase11_db_executor_completed_total " << queue.completed << '\n'
                     << "webserver_phase11_db_executor_failed_total " << queue.failed << '\n'
                     << "# TYPE webserver_phase11_db_executor_active gauge\n"
                     << "webserver_phase11_db_executor_active " << queue.active << '\n'
                     << "webserver_phase11_db_executor_queued " << queue.queued << '\n'
                     << "webserver_phase11_db_executor_queue_high_watermark "
                     << queue.queueHighWatermark << '\n'
                     << "webserver_phase11_db_executor_capacity " << queue.capacity << '\n'
                     << "# TYPE webserver_phase11_file_executor_active gauge\n"
                     << "webserver_phase11_file_executor_submitted_total " << files.submitted << '\n'
                     << "webserver_phase11_file_executor_rejected_total " << files.rejected << '\n'
                     << "webserver_phase11_file_executor_failed_total " << files.failed << '\n'
                     << "webserver_phase11_file_executor_active " << files.active << '\n'
                     << "webserver_phase11_file_executor_queued " << files.queued << '\n'
                     << "webserver_phase11_file_executor_queue_high_watermark "
                     << files.queueHighWatermark << '\n'
                     << "# TYPE webserver_phase11_rag_executor_active gauge\n"
                     << "webserver_phase11_rag_executor_submitted_total "
                     << knowledge.submitted << '\n'
                     << "webserver_phase11_rag_executor_rejected_total "
                     << knowledge.rejected << '\n'
                     << "webserver_phase11_rag_executor_failed_total "
                     << knowledge.failed << '\n'
                     << "webserver_phase11_rag_executor_active "
                     << knowledge.active << '\n'
                     << "webserver_phase11_rag_executor_queued "
                     << knowledge.queued << '\n'
                     << "webserver_phase11_rag_executor_queue_high_watermark "
                     << knowledge.queueHighWatermark << '\n'
                     << "# TYPE webserver_phase11_postgres_pool_in_use gauge\n"
                     << "webserver_phase11_postgres_pool_size " << pool.size << '\n'
                     << "webserver_phase11_postgres_pool_available " << pool.available << '\n'
                     << "webserver_phase11_postgres_pool_in_use " << pool.inUse << '\n'
                     << "webserver_phase11_postgres_pool_in_use_high_watermark "
                     << pool.inUseHighWatermark << '\n'
                     << "webserver_phase11_postgres_pool_acquires_total " << pool.acquires << '\n'
                     << "webserver_phase11_postgres_pool_waited_acquires_total "
                     << pool.waitedAcquires << '\n'
                     << "webserver_phase11_postgres_pool_timeouts_total " << pool.timeouts << '\n'
                     << "webserver_phase11_postgres_pool_acquire_wait_microseconds_total "
                     << pool.totalAcquireWaitMicroseconds << '\n'
                     << "# TYPE webserver_phase11_outbox_published_total counter\n"
                     << "webserver_phase11_outbox_cycles_total " << outbox.cycles << '\n'
                     << "webserver_phase11_outbox_wakeups_total " << outbox.wakeups << '\n'
                     << "webserver_phase11_outbox_attempted_total " << outbox.attempted << '\n'
                     << "webserver_phase11_outbox_published_total " << outbox.published << '\n'
                     << "webserver_phase11_outbox_blocked_total " << outbox.blocked << '\n'
                     << "webserver_phase11_outbox_failures_total " << outbox.failures << '\n'
                     << "# TYPE webserver_phase11_outbox_last_batch gauge\n"
                     << "webserver_phase11_outbox_last_batch " << outbox.lastBatch << '\n'
                     << "webserver_phase11_outbox_batch_high_watermark "
                     << outbox.batchHighWatermark << '\n'
                     << "webserver_phase11_outbox_oldest_attempt_age_microseconds "
                     << outbox.oldestAttemptAgeMicroseconds << '\n'
                     << "# TYPE webserver_phase11_ai_requests_total counter\n"
                     << "webserver_phase11_ai_accepted_total " << aiChat.accepted << '\n'
                     << "webserver_phase11_ai_quota_rejected_total "
                     << aiChat.quotaRejected << '\n'
                     << "webserver_phase11_ai_succeeded_total " << aiChat.succeeded << '\n'
                     << "webserver_phase11_ai_failed_total " << aiChat.failed << '\n'
                     << "webserver_phase11_ai_cancelled_total " << aiChat.cancelled << '\n'
                     << "webserver_phase11_ai_timed_out_total " << aiChat.timedOut << '\n'
                     << "webserver_phase11_ai_audit_snapshots_queued_total "
                     << aiChat.auditSnapshotsQueued << '\n'
                     << "webserver_phase11_ai_audit_enqueue_rejected_total "
                     << aiChat.auditEnqueueRejected << '\n'
                     << "# TYPE webserver_phase11_ai_active gauge\n"
                     << "webserver_phase11_ai_active " << aiChat.active << '\n'
                     << "webserver_phase11_ai_provider_requests_total "
                     << aiProviders.requests << '\n'
                     << "webserver_phase11_ai_provider_primary_failures_total "
                     << aiProviders.primaryFailures << '\n'
                     << "webserver_phase11_ai_provider_fallback_attempts_total "
                     << aiProviders.fallbackAttempts << '\n'
                     << "webserver_phase11_ai_provider_fallback_successes_total "
                     << aiProviders.fallbackSuccesses << '\n'
                     << "webserver_phase11_ai_provider_fallback_failures_total "
                     << aiProviders.fallbackFailures << '\n'
                     << "webserver_phase11_ai_provider_circuit_opened_total "
                     << aiProviders.circuitOpened << '\n'
                     << "webserver_phase11_ai_provider_circuit_rejected_total "
                     << aiProviders.circuitRejected << '\n'
                     << "webserver_phase11_ai_provider_timed_out_total "
                     << aiProviders.timedOut << '\n'
                     << "webserver_phase11_ai_provider_active "
                     << aiProviders.active << '\n';
                constexpr std::array<std::string_view, 12> latencyLabels{
                    "0.01", "0.025", "0.05", "0.1", "0.25", "0.5",
                    "1", "2.5", "5", "10", "30", "60"};
                const auto appendHistogram = [&](std::string_view name,
                                                 const auto &buckets,
                                                 std::uint64_t count,
                                                 std::uint64_t micros)
                {
                    text << "# TYPE " << name << " histogram\n";
                    for (std::size_t index = 0; index < latencyLabels.size();
                         ++index)
                        text << name << "_bucket{le=\"" << latencyLabels[index]
                             << "\"} " << buckets[index] << '\n';
                    text << name << "_bucket{le=\"+Inf\"} "
                         << buckets.back() << '\n'
                         << name << "_sum "
                         << static_cast<double>(micros) / 1'000'000.0 << '\n'
                         << name << "_count " << count << '\n';
                };
                appendHistogram(
                    "webserver_phase11_ai_first_token_latency_seconds",
                    aiChat.firstTokenLatencyBuckets,
                    aiChat.firstTokenLatencyCount,
                    aiChat.firstTokenLatencyMicrosecondsTotal);
                appendHistogram(
                    "webserver_phase11_ai_total_latency_seconds",
                    aiChat.totalLatencyBuckets, aiChat.totalLatencyCount,
                    aiChat.totalLatencyMicrosecondsTotal);
                return text.str();

}
}
