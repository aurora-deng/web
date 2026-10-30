#pragma once
class ServerRuntime;
namespace webserver::learning {
/** Existing tutorial HTTP/SSE/WebSocket handlers; no persistent business rules. */
void registerLearningRoutes(ServerRuntime &server);
}
