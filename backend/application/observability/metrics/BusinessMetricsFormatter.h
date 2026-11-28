#pragma once
#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/runtime/BoundedExecutor/BoundedExecutor.h"
#include "backend/application/modules/ai/application/AiChatService/AiChatService.h"
#include "backend/application/modules/chat/transport/Phase11Realtime/Phase11Realtime.h"
#include "backend/ai/inference/resilience/ResilientModelProvider/ResilientModelProvider.h"
namespace webserver::phase11 {
/** Snapshot formatting only; cumulative SQL time is not request wall time. */
std::string formatBusinessMetrics(PostgresStore *store, PostgresStore *deliveryStore, DatabaseExecutor *executor, BoundedExecutor *fileExecutor, BoundedExecutor *knowledgeExecutor, transport::Phase11OutboxPump *pump, AiChatService *ai, const std::vector<std::shared_ptr<ResilientModelProvider>> &resilient, BoundedExecutor *aiExecutor = nullptr);
}
