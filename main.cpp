#include "backend/application/transport/learning/LearningRoutes/LearningRoutes.h"
#include "backend/application/modules/chat/bootstrap/ChatWorkspaceModule/ChatWorkspaceModule.h"
#include "backend/application/runtime/BusinessShutdown/BusinessShutdown.h"
#include "backend/application/observability/BusinessMetrics/BusinessMetrics.h"
#include "backend/application/config/RuntimeConfig/RuntimeConfig.h"
#include "ProjectBuild.h"
#include "backend/application/modules/shared/transport/FrontendAssets/FrontendAssets.h"
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
#include "backend/application/modules/bootstrap/Function3Runtime/Function3Runtime.h"
#endif
// ============================================================
// 文件名：main.cpp
// ------------------------------------------------------------
// 【职责比喻：工厂大门 + 前台】
//   整个服务器就像一座工厂：本文件既是"大门"也是"前台"——开门投产前，
//   前台要先把各项业务登记到"服务手册"（路由表 Router）上：访客点 "/"
//   就上"hello"这道菜，点 "/user/:id" 就报出访客编号，点 "/ws" 就把访客
//   领进 WebSocket 包间。菜单写好后，前台按下"开工"按钮（server.start()），
//   车间（SubReactor 线程）、工人（协程）、搬运工（Executor 线程池）就开始
//   各司其职运转起来。如果开门失败（比如端口被占），前台要体面地关门并
//   告诉原因，而不是当场崩溃（避免 core dump）。
//
// 关键技术点（初学者重点理解）：
//   1. 中间件链（middleware）：像安检传送带，请求依次经过"日志→鉴权"再到达
//      业务 handler；任意一环调用 next() 放行，不调用则短路返回。
//   2. 动态路由参数："/user/:id" 中的 :id 是占位符，框架自动把 URL 里的实际
//      值填进 ctx.params，handler 用 ctx.params.at("id") 取出。
//   3. Executor 异步执行：/slow 的耗时循环不会卡死整个服务器，因为 handler
//      被丢到 HTTP Worker 池执行；循环还会轮询撤单信号，避免超时后继续白干。
//   4. WebSocket 升级握手：/ws 是 WebSocket 握手入口。客户端发起 HTTP Upgrade
//      请求到 /ws，业务层调用 ctx.acceptWebSocket() 标记此连接要升级为
//      WebSocket；真正的握手应答（101 Switching Protocols + Sec-WebSocket-Accept）
//      和后续帧收发由框架的 WebSocketSession 接管，体现协议边界的清晰划分。
// ============================================================
#include "backend/server/Runtime/ServerRuntime.h"
#include "backend/server/http/RequestContext/RequestContext.h"
#include "backend/server/sse/SseEvent.h"
#include "backend/server/ops/OperationalMetrics.h"
#include "backend/server/security/AuthToken.h"
#include "backend/server/security/FixedWindowRateLimiter.h"
#include "backend/server/websocket/WebSocketCodec/WebSocketCodec.h"
#include "backend/server/websocket/WebSocketDelivery/WebSocketDeliveryService.h"
#include "backend/server/websocket/WebSocketDispatcher/WsMessageContext.h"
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
#include "backend/ai/models/ModelEvaluationService/ModelEvaluationService.h"
#include "backend/ai/models/ModelRegistryService/ModelRegistryService.h"
#include "backend/ai/bootstrap/ModelProviderAssembly/ModelProviderAssembly.h"
#include "backend/ai/models/DeterministicKnowledgeIndex/DeterministicKnowledgeIndex.h"
#include "backend/ai/models/RagService/RagService.h"
#include "backend/ai/models/ResilientModelProvider/ResilientModelProvider.h"
#ifdef WEBSERVER_HAS_CURL_CLIENT
#include "backend/ai/models/OllamaModelProvider/OllamaModelProvider.h"
#endif
#ifdef WEBSERVER_HAS_ONNX_GENAI
#include "backend/ai/models/InProcessOnnxModelProvider/InProcessOnnxModelProvider.h"
#endif
#ifdef WEBSERVER_HAS_PHASE11_GRPC_ONNX
#include "backend/ai/models/GrpcOnnxModelProvider/GrpcOnnxModelProvider.h"
#endif
#include "backend/application/business/AiChatService/AiChatService.h"
#include "backend/ai/private/infrastructure/PrivateModelRouter/PrivateModelRouter.h"
#include "backend/ai/private/transport/PrivateGenerationHttp/PrivateGenerationHttp.h"
#include "backend/application/business/AuthService/AuthService.h"
#include "backend/application/business/ChatService/ChatService.h"
#include "backend/application/business/FileService/FileService.h"
#include "backend/application/business/SocialService/SocialService.h"
#include "backend/application/runtime/BoundedExecutor/BoundedExecutor.h"
#include "backend/application/security/InMemoryLoginRateLimiter/InMemoryLoginRateLimiter.h"
#include "backend/application/security/SodiumCredentialCodec/SodiumCredentialCodec.h"
#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/observability/BusinessMetricsFormatter/BusinessMetricsFormatter.h"
#include "backend/application/modules/messaging/infrastructure/ChatOutboxWriter/ChatOutboxWriter.h"
#include "backend/application/storage/files/LocalFileStorage/LocalFileStorage.h"
#include "backend/ai/training/TrainingDataService/TrainingDataService.h"
#include "backend/application/transport/http/Phase11AuthHttp/Phase11AuthHttp.h"
#include "backend/application/transport/http/Phase11FileHttp/Phase11FileHttp.h"
#include "backend/application/transport/http/Phase11KnowledgeHttp/Phase11KnowledgeHttp.h"
#include "backend/application/transport/http/Phase11SocialChatHttp/Phase11SocialChatHttp.h"
#include "backend/application/transport/http/Phase11TrainingHttp/Phase11TrainingHttp.h"
#include "backend/application/transport/realtime/Phase11Realtime/Phase11Realtime.h"
#endif
#include "backend/server/observability/logger/logger.h"
#ifdef WEBSERVER_HAS_GRPC
#include "backend/server/grpc/GrpcServer.h"
#endif
#include <unistd.h>
#include <cerrno>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace
{
bool environmentFlag(const char *name){return webserver::config::RuntimeConfig::flag(name);}
std::size_t boundedEnvironmentSize(const char*name,std::size_t fallback,std::size_t low,std::size_t high){return webserver::config::RuntimeConfig::size(name,fallback,low,high);}

#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
std::string readPrivateHexKey(const char *path)
{
    if (!path || *path == '\0')
        throw std::invalid_argument("WEB_PHASE11_TOKEN_KEY_FILE is required");

    struct stat metadata{};
    if (::stat(path, &metadata) != 0 || !S_ISREG(metadata.st_mode))
        throw std::invalid_argument(
            "WEB_PHASE11_TOKEN_KEY_FILE must name a readable regular file");
    if ((metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        throw std::invalid_argument(
            "WEB_PHASE11_TOKEN_KEY_FILE must not grant group/other permissions");

    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::invalid_argument("cannot read WEB_PHASE11_TOKEN_KEY_FILE");
    std::string key{std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>()};
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r'))
        key.pop_back();
    if (key.size() != 64)
        throw std::invalid_argument(
            "WEB_PHASE11_TOKEN_KEY_FILE must contain 64 hexadecimal characters");
    return key;
}

#endif

std::unordered_set<std::string> commaSeparatedSet(const char *raw)
{
    std::unordered_set<std::string> values;
    if (!raw)
        return values;
    std::string_view input(raw);
    std::size_t start = 0;
    while (start <= input.size())
    {
        const auto end = input.find(',', start);
        auto item = input.substr(start, end == std::string_view::npos
                                           ? input.size() - start
                                           : end - start);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
            item.remove_prefix(1);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
            item.remove_suffix(1);
        if (!item.empty())
            values.emplace(item);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return values;
}

bool protectedPath(std::string_view path)
{
    return path == "/ws" || path == "/events" ||
           path.starts_with("/events/") || path == "/events-status" ||
           path == "/delivery-metrics" || path == "/metrics" ||
           path == "/admin";
}

bool operationsPath(const HttpRequest &request)
{
    return request.path == "/metrics" || request.path == "/delivery-metrics" ||
           request.path == "/events-status" ||
           (request.method == "POST" && request.path.starts_with("/events/"));
}

void reject(RequestContext &ctx, int status, std::string statusText,
            const std::string &message)
{
    ctx.response->status = status;
    ctx.response->statusText = std::move(statusText);
    ctx.response->json("{\"error\":\"" + message + "\"}");
    ctx.response->setHeader("Cache-Control", "no-store");
}

bool queryUserMatches(const RequestContext &ctx, std::uint64_t userId)
{
    const auto found = ctx.request.querryParams.find("uid");
    if (found == ctx.request.querryParams.end() || found->second.empty())
        return true;
    try
    {
        std::size_t parsed = 0;
        const auto supplied = std::stoull(found->second, &parsed);
        return parsed == found->second.size() && supplied == userId;
    }
    catch (...)
    {
        return false;
    }
}


}

/**
 * @brief 服务器主入口：装配路由与中间件，启动事件循环
 *
 * 【工厂开门投产】
 *   1. 先创建一个 ServerRuntime（相当于工厂整体运营环境，内含 Reactor、
 *      线程池、定时器等基础设施）。
 *   2. 在 router() 上挂中间件和路由（相当于登记业务菜单和安检流程）。
 *   3. 调用 server.start() 正式开工，主线程进入事件循环阻塞等待。
 *   4. 任何启动异常（如端口占用）用 try/catch 兜底，返回非零退出码。
 *
 * @return 0 正常退出；1 启动失败（如端口被占用）
 */
int main()
{
    try
    {
        const auto runtimeConfig=webserver::config::RuntimeConfig::load();
        const bool productionMode = runtimeConfig.production;
        const std::string authSecret = std::getenv("WEB_AUTH_SECRET")
                                           ? std::getenv("WEB_AUTH_SECRET")
                                           : "";
        if (productionMode && authSecret.size() < 32)
            throw std::invalid_argument(
                "WEB_PRODUCTION_MODE requires WEB_AUTH_SECRET with at least 32 bytes");
        auto authTokens = std::make_shared<webserver::security::AuthToken>(authSecret);
        auto allowedOrigins = std::make_shared<const std::unordered_set<std::string>>(
            commaSeparatedSet(std::getenv("WEB_ALLOWED_ORIGINS")));
        if (productionMode && allowedOrigins->empty())
            throw std::invalid_argument(
                "WEB_PRODUCTION_MODE requires WEB_ALLOWED_ORIGINS");
        auto operationalMetrics =
            std::make_shared<webserver::ops::OperationalMetrics>();
        auto rateLimiter = std::make_shared<webserver::security::FixedWindowRateLimiter>(
            boundedEnvironmentSize("WEB_REQUESTS_PER_MINUTE", 120, 1, 1'000'000),
            std::chrono::minutes(1),
            boundedEnvironmentSize("WEB_RATE_LIMIT_IDENTITIES", 65'536, 1, 10'000'000));

#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        // 这些对象先于 ServerRuntime 声明，因此退出时 ServerRuntime 会先停止网络线程，
        // 然后认证适配器、数据库执行器和连接池才依次销毁，避免路由捕获悬空对象。
        std::unique_ptr<webserver::chat::ChatWorkspaceModule> phase11Workspace;
        std::unique_ptr<webserver::phase11::PostgresStore> phase11Store;
        std::unique_ptr<webserver::phase11::PostgresStore> phase11DeliveryStore;
        std::unique_ptr<webserver::phase11::SodiumCredentialCodec> phase11Credentials;
        std::unique_ptr<webserver::phase11::InMemoryLoginRateLimiter>
            phase11LoginLimiter;
        std::unique_ptr<webserver::phase11::AuthService> phase11Auth;
        std::unique_ptr<webserver::phase11::SocialService> phase11Social;
        std::unique_ptr<webserver::phase11::ChatService> phase11Chat;
        std::unique_ptr<webserver::phase11::ModelEvaluationService>
            phase11ModelEvaluations;
        std::unique_ptr<webserver::phase11::ModelRegistryService>
            phase11ModelRegistry;
        std::unique_ptr<webserver::phase11::DeterministicKnowledgeIndex>
            phase11KnowledgeIndex;
        std::unique_ptr<webserver::phase11::RagService> phase11Rag;
        std::unique_ptr<webserver::phase11::TrainingDataService>
            phase11Training;
        std::unique_ptr<webserver::phase11::LocalFileStorage> phase11FileStorage;
        std::unique_ptr<webserver::phase11::FileService> phase11Files;
        std::unique_ptr<webserver::phase11::DatabaseExecutor> phase11DatabaseExecutor;
        std::unique_ptr<webserver::phase11::BoundedExecutor> phase11FileExecutor;
        std::unique_ptr<webserver::phase11::BoundedExecutor>
            phase11KnowledgeExecutor;
        std::unique_ptr<webserver::phase11::transport::Phase11AuthHttp>
            phase11AuthHttp;
        std::unique_ptr<webserver::phase11::transport::Phase11SocialChatHttp>
            phase11SocialChatHttp;
        std::unique_ptr<webserver::phase11::transport::Phase11FileHttp>
            phase11FileHttp;
        std::unique_ptr<webserver::phase11::transport::Phase11KnowledgeHttp>
            phase11KnowledgeHttp;
        std::unique_ptr<webserver::phase11::transport::Phase11TrainingHttp>
            phase11TrainingHttp;
#endif

        const ServerRuntimeOptions runtimeOptions{
            boundedEnvironmentSize("WEB_HTTP_WORKERS", 0, 1, 32),
            boundedEnvironmentSize("WEB_WS_WORKERS", 0, 1, 32)};
        ServerRuntime server(runtimeOptions);
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        // 这些实时适配器依赖 ServerRuntime 内的 WS/SSE 目录，因此在 server 之后声明；
        // 声明顺序也规定退出顺序：先停 Outbox，再停 AI 编排，然后销毁 Provider/SSE。
        std::unique_ptr<webserver::phase11::transport::Phase11AiEventSink>
            phase11AiEvents;
        std::unique_ptr<webserver::phase11::ModelRouter> phase11ModelRouter;
        std::vector<std::shared_ptr<webserver::phase11::IModelProvider>>
            phase11ModelProviders;
        std::vector<std::shared_ptr<webserver::phase11::ResilientModelProvider>>
            phase11ResilientProviders;
        std::unique_ptr<webserver::phase11::AiChatService> phase11AiChat;
        std::unique_ptr<webserver::phase11::PrivateModelRouter> phase11PrivateModel;
        std::unique_ptr<webserver::phase11::PrivateGenerationService> phase11PrivateGenerations;
        std::unique_ptr<webserver::phase11::PrivateGenerationService> phase11WebGenerations;
        std::unique_ptr<webserver::phase11::transport::PrivateGenerationHttp> phase11PrivateHttp;
        std::unique_ptr<webserver::phase11::transport::PrivateGenerationHttp> phase11WebHttp;
        std::unique_ptr<webserver::phase11::transport::Phase11Realtime>
            phase11Realtime;
        std::unique_ptr<webserver::phase11::transport::Phase11RealtimeOutboxSink>
            phase11OutboxSink;
        std::unique_ptr<webserver::function3::Function3Runtime> function3Runtime;
        std::unique_ptr<webserver::phase11::transport::Phase11OutboxPump>
            phase11OutboxPump;
        std::unique_ptr<webserver::phase11::transport::Phase11PresenceBridge>
            phase11PresenceBridge;
#endif
        std::function<std::string()> phase11MetricsText = [] { return std::string{}; };
        std::function<bool()> phase11Ready = [] { return true; };
        std::cout << "worker pools: http=" << server.httpWorkerCount()
                  << ", websocket=" << server.webSocketWorkerCount() << '\n';
        server.setMaxConnections(
            boundedEnvironmentSize("WEB_MAX_CONNECTIONS", 10'000, 1, 1'000'000));
        server.setShutdownDrain(std::chrono::milliseconds(
            boundedEnvironmentSize("WEB_SHUTDOWN_DRAIN_MS", 500, 0, 30'000)));
        if (const char *rawPort = std::getenv("WEB_SERVER_PORT")) {
            char *end = nullptr;
            errno = 0;
            const long value = std::strtol(rawPort, &end, 10);
            if (errno || end == rawPort || *end != '\0' || value < 1 || value > 65535)
                throw std::invalid_argument("WEB_SERVER_PORT must be in [1, 65535]");
            server.setPort(static_cast<int>(value));
        }
        // 同时设置证书和私钥才开启独立 HTTPS 端口。ALPN 在此端口选 h2/http/1.1；
        // 8080 明文入口仍保留 HTTP/1.1 与 prior-knowledge h2c。
        if (const char *cert = std::getenv("WEB_TLS_CERT")) {
            const char *key = std::getenv("WEB_TLS_KEY");
            if (!key)
                throw std::invalid_argument("WEB_TLS_KEY is required with WEB_TLS_CERT");
            int tlsPort = 8443;
            if (const char *rawPort = std::getenv("WEB_TLS_PORT")) {
                char *end = nullptr;
                errno = 0;
                const long value = std::strtol(rawPort, &end, 10);
                if (errno || end == rawPort || *end != '\0' || value < 1 || value > 65535)
                    throw std::invalid_argument("WEB_TLS_PORT must be in [1, 65535]");
                tlsPort = static_cast<int>(value);
            }
            server.setTls(cert, key, tlsPort);
        } else if (std::getenv("WEB_TLS_KEY")) {
            throw std::invalid_argument("WEB_TLS_CERT is required with WEB_TLS_KEY");
        }
        if (productionMode && !std::getenv("WEB_TLS_CERT"))
            throw std::invalid_argument(
                "WEB_PRODUCTION_MODE requires WEB_TLS_CERT and WEB_TLS_KEY");

#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        const char *phase11Database = std::getenv("WEB_PHASE11_DATABASE");
        const char *phase11KeyFile = std::getenv("WEB_PHASE11_TOKEN_KEY_FILE");
        if ((phase11Database && *phase11Database) !=
            (phase11KeyFile && *phase11KeyFile))
            throw std::invalid_argument(
                "WEB_PHASE11_DATABASE and WEB_PHASE11_TOKEN_KEY_FILE must be set together");
        if (phase11Database && *phase11Database)
        {
            const auto sessionLifetime = std::chrono::seconds(
                boundedEnvironmentSize("WEB_PHASE11_SESSION_SECONDS", 86'400,
                                       60, 31'536'000));
            phase11Store = std::make_unique<webserver::phase11::PostgresStore>(
                webserver::phase11::PostgresPoolConfig{
                    phase11Database,
                    boundedEnvironmentSize("WEB_PHASE11_DB_POOL", 4, 1, 32),
                    std::chrono::milliseconds(
                        boundedEnvironmentSize("WEB_PHASE11_DB_ACQUIRE_MS",
                                               5'000, 10, 60'000))});
            if (!phase11Store->healthy())
                throw std::runtime_error("Phase 11 PostgreSQL health check failed");
            phase11Store->requireMigrations(24);
            phase11ModelEvaluations = std::make_unique<
                webserver::phase11::ModelEvaluationService>(
                    *phase11Store, *phase11Store);
            phase11ModelRegistry = std::make_unique<
                webserver::phase11::ModelRegistryService>(
                    *phase11Store, phase11ModelEvaluations.get());
            phase11Credentials =
                std::make_unique<webserver::phase11::SodiumCredentialCodec>(
                    readPrivateHexKey(phase11KeyFile));
            phase11LoginLimiter = std::make_unique<
                webserver::phase11::InMemoryLoginRateLimiter>();
            phase11Auth = std::make_unique<webserver::phase11::AuthService>(
                *phase11Store, *phase11Store, *phase11Credentials,
                *phase11LoginLimiter, sessionLifetime);
            phase11Social = std::make_unique<webserver::phase11::SocialService>(
                *phase11Store, *phase11Store, *phase11Store, *phase11Store);
            phase11Chat = std::make_unique<webserver::phase11::ChatService>(
                *phase11Store, *phase11Store, *phase11Store, *phase11Store,
                *phase11Store, *phase11Store, *phase11Store);
            phase11Training = std::make_unique<
                webserver::phase11::TrainingDataService>(
                    *phase11Store, *phase11Store, *phase11Store,
                    *phase11Store, *phase11Store);
            phase11KnowledgeIndex = std::make_unique<
                webserver::phase11::DeterministicKnowledgeIndex>();
            webserver::phase11::RagServiceConfig ragConfig;
            ragConfig.maxDocumentBytes = boundedEnvironmentSize(
                "WEB_PHASE11_RAG_DOCUMENT_BYTES", 2 * 1024 * 1024,
                1024, 16 * 1024 * 1024);
            ragConfig.maxChunkBytes = boundedEnvironmentSize(
                "WEB_PHASE11_RAG_CHUNK_BYTES", 1200, 256, 8192);
            ragConfig.overlapBytes = boundedEnvironmentSize(
                "WEB_PHASE11_RAG_OVERLAP_BYTES", 160, 0, 4095);
            ragConfig.maxRetrievedChunks = boundedEnvironmentSize(
                "WEB_PHASE11_RAG_MAX_CHUNKS", 8, 1, 32);
            phase11Rag = std::make_unique<webserver::phase11::RagService>(
                *phase11Store, *phase11Store, *phase11Store,
                *phase11KnowledgeIndex, ragConfig);
            phase11DatabaseExecutor = std::make_unique<
                webserver::phase11::DatabaseExecutor>(
                    boundedEnvironmentSize("WEB_PHASE11_DB_WORKERS", 4, 1, 32),
                    boundedEnvironmentSize("WEB_PHASE11_DB_QUEUE", 256, 1,
                                           65'536));
            const std::string fileRoot =
                std::getenv("WEB_PHASE11_FILE_ROOT")
                    ? std::getenv("WEB_PHASE11_FILE_ROOT")
                    : "phase11-files";
            phase11FileStorage = std::make_unique<
                webserver::phase11::LocalFileStorage>(fileRoot);
            phase11Files = std::make_unique<webserver::phase11::FileService>(
                *phase11Store, *phase11Store, *phase11Store,
                *phase11FileStorage);
            phase11FileExecutor = std::make_unique<
                webserver::phase11::BoundedExecutor>(
                    boundedEnvironmentSize("WEB_PHASE11_FILE_WORKERS", 2, 1, 16),
                    boundedEnvironmentSize("WEB_PHASE11_FILE_QUEUE", 64, 1,
                                           4096));
            // 文档切片和索引重建可能明显慢于普通 CRUD，使用独立有界队列，避免
            // 挤占数据库查询和文件传输任务。
            phase11KnowledgeExecutor = std::make_unique<
                webserver::phase11::BoundedExecutor>(
                    boundedEnvironmentSize("WEB_PHASE11_RAG_WORKERS", 2, 1, 16),
                    boundedEnvironmentSize("WEB_PHASE11_RAG_QUEUE", 32, 1,
                                           4096));
            webserver::phase11::transport::Phase11AuthHttpConfig httpConfig;
            httpConfig.secureCookie = productionMode ||
                                      environmentFlag("WEB_PHASE11_SECURE_COOKIE");
            httpConfig.sessionLifetime = sessionLifetime;
            if (const char *header =
                    std::getenv("WEB_PHASE11_TRUSTED_CLIENT_IP_HEADER"))
                httpConfig.trustedClientIpHeader = header;
            phase11AuthHttp = std::make_unique<
                webserver::phase11::transport::Phase11AuthHttp>(
                    *phase11Auth, *phase11DatabaseExecutor,
                    std::move(httpConfig));
            if (environmentFlag("WEB_FUNCTION3_ENABLE"))
                function3Runtime = std::make_unique<webserver::function3::Function3Runtime>(
                    phase11Database, *phase11AuthHttp, *phase11DatabaseExecutor, server.sseManager(), *phase11Files, *phase11Social, server.wsManager(),
                    *phase11Store, *phase11ModelRegistry, *phase11ModelEvaluations,[index=phase11KnowledgeIndex.get()](std::uint64_t document){index->removeDocument(document);});
            phase11AiEvents = std::make_unique<
                webserver::phase11::transport::Phase11AiEventSink>(
                    server.sseManager());
            phase11ModelRouter =
                std::make_unique<webserver::phase11::ModelRouter>(*phase11Store);
            webserver::phase11::configureModelProviders(
                *phase11ModelRouter,*phase11ModelRegistry,
                phase11ModelProviders,phase11ResilientProviders,productionMode);
            webserver::phase11::AiChatServiceConfig aiChatConfig;
            aiChatConfig.maxActiveGenerations = boundedEnvironmentSize(
                "WEB_PHASE11_AI_MAX_ACTIVE", 64, 1, 65'536);
            aiChatConfig.maxActivePerUser = boundedEnvironmentSize(
                "WEB_PHASE11_AI_MAX_PER_USER", 2, 1, 1024);
            aiChatConfig.maxActivePerAiAccount = boundedEnvironmentSize(
                "WEB_PHASE11_AI_MAX_PER_ACCOUNT", 16, 1, 65'536);
            aiChatConfig.generationTimeout = std::chrono::milliseconds(
                boundedEnvironmentSize("WEB_PHASE11_AI_DEADLINE_MS",
                                       330'000, 100, 3'600'000));
            aiChatConfig.ragMaxChunks = boundedEnvironmentSize(
                "WEB_PHASE11_AI_RAG_MAX_CHUNKS", 6, 1,
                ragConfig.maxRetrievedChunks);
            phase11AiChat = std::make_unique<webserver::phase11::AiChatService>(
                *phase11Chat, *phase11Store, *phase11Store,
                *phase11ModelRouter, *phase11DatabaseExecutor,
                *phase11Store, *phase11AiEvents, aiChatConfig,
                phase11Rag.get());
            phase11PrivateModel=std::make_unique<webserver::phase11::PrivateModelRouter>(*phase11ModelRouter,*phase11Store);
            // 网页非保密任务单独拥有任务表：HTTP 网页接口不能读取 HTTPS 私人任务。
            phase11WebGenerations=std::make_unique<webserver::phase11::PrivateGenerationService>(*phase11PrivateModel);
            phase11WebHttp=std::make_unique<webserver::phase11::transport::PrivateGenerationHttp>(*phase11AuthHttp,*phase11DatabaseExecutor,*phase11WebGenerations,true);
            phase11PrivateGenerations=std::make_unique<webserver::phase11::PrivateGenerationService>(*phase11PrivateModel);
            phase11PrivateHttp=std::make_unique<webserver::phase11::transport::PrivateGenerationHttp>(*phase11AuthHttp,*phase11DatabaseExecutor,*phase11PrivateGenerations);
            // MQ 消费者的历史/成员/通知偏好查询使用独立小池，避免与聊天提交争抢连接。
            // 仍访问同一数据库，不复制业务数据；两个池都保留明确等待期限。
            if(function3Runtime)phase11DeliveryStore=std::make_unique<webserver::phase11::PostgresStore>(
                webserver::phase11::PostgresPoolConfig{phase11Database,
                    boundedEnvironmentSize("WEB_FUNCTION3_DELIVERY_DB_POOL",2,1,8),std::chrono::milliseconds(2000)});
            auto&deliveryStore=phase11DeliveryStore?*phase11DeliveryStore:*phase11Store;
            if(function3Runtime){const char*endpoint=std::getenv("WEB_FUNCTION3_MQ_ENDPOINT");
                if(endpoint&&*endpoint)phase11Store->setOutboxWriter(std::make_shared<webserver::function3::ChatOutboxWriter>(function3Runtime->commitNotifier()));}
            phase11OutboxSink = std::make_unique<
                webserver::phase11::transport::Phase11RealtimeOutboxSink>(
                    deliveryStore, deliveryStore, deliveryStore,
                    server.wsManager(),
                    server.sseManager());
            phase11OutboxPump = std::make_unique<
                webserver::phase11::transport::Phase11OutboxPump>(
                    *phase11Store, function3Runtime
                        ? function3Runtime->startMessaging(*phase11OutboxSink, deliveryStore)
                        : *phase11OutboxSink);
            const bool notifyOutboxAfterCommit =
                !environmentFlag("WEB_PHASE11_OUTBOX_POLL_ONLY");
            const auto notifyOutbox =
                [pump = phase11OutboxPump.get(), notifyOutboxAfterCommit]
            {
                if (notifyOutboxAfterCommit)
                    pump->notifyCommitted();
            };
            phase11SocialChatHttp = std::make_unique<
                webserver::phase11::transport::Phase11SocialChatHttp>(
                    *phase11AuthHttp, *phase11Social, *phase11Chat,
                    *phase11DatabaseExecutor, server.wsManager(), notifyOutbox);
            phase11Workspace=std::make_unique<webserver::chat::ChatWorkspaceModule>(
                *phase11AuthHttp,*phase11DatabaseExecutor,*phase11Store,*phase11Store,
                *phase11Store,*phase11Store,*phase11Store,*phase11Store,notifyOutbox);
            phase11FileHttp = std::make_unique<
                webserver::phase11::transport::Phase11FileHttp>(
                    *phase11AuthHttp, *phase11Files, *phase11Chat,
                    *phase11FileExecutor, notifyOutbox);
            webserver::phase11::transport::Phase11KnowledgeHttpConfig
                knowledgeHttpConfig;
            knowledgeHttpConfig.maximumRequestBytes =
                ragConfig.maxDocumentBytes * 2U + 64U * 1024U;
            phase11KnowledgeHttp = std::make_unique<
                webserver::phase11::transport::Phase11KnowledgeHttp>(
                    *phase11AuthHttp, *phase11Rag,
                    *phase11KnowledgeExecutor, knowledgeHttpConfig);
            phase11TrainingHttp = std::make_unique<
                webserver::phase11::transport::Phase11TrainingHttp>(
                    *phase11AuthHttp, *phase11Training,
                    *phase11DatabaseExecutor);
            phase11Realtime = std::make_unique<
                webserver::phase11::transport::Phase11Realtime>(
                    *phase11Chat, *phase11DatabaseExecutor,
                    phase11AiChat.get(), notifyOutbox);
            phase11Realtime->registerHandlers(server.wsDispatcher());
            phase11PresenceBridge = std::make_unique<
                webserver::phase11::transport::Phase11PresenceBridge>(
                    *phase11Social, *phase11DatabaseExecutor,
                    server.wsManager(), notifyOutbox);
            phase11MetricsText = [store = phase11Store.get(),
                                  deliveryStore = phase11DeliveryStore.get(),
                                  executor = phase11DatabaseExecutor.get(),
                                  fileExecutor = phase11FileExecutor.get(),
                                  knowledgeExecutor =
                                      phase11KnowledgeExecutor.get(),
                                  pump = phase11OutboxPump.get(),
                                  ai = phase11AiChat.get(),
                                  resilient = phase11ResilientProviders]
            {
                return webserver::phase11::formatBusinessMetrics(store,deliveryStore,executor,fileExecutor,knowledgeExecutor,pump,ai,resilient);
            };
            // Readiness is a dependency check, not only a listening-socket check.
            // It runs on the HTTP worker lane below, so a slow database lease never
            // blocks the owning Reactor thread.
            phase11Ready = [store = phase11Store.get()]
            {
                return store->healthy();
            };
            std::cout << "Phase 11 authentication enabled\n";
        }
        else if (environmentFlag("WEB_PHASE11_REQUIRED"))
            throw std::invalid_argument(
                "WEB_PHASE11_REQUIRED needs database and token-key configuration");
#endif
        // 可重复的并发测试需要固定 Reactor 数；生产环境不设置时仍按 CPU 自动选择。
        if (const char *raw = std::getenv("WEB_SERVER_REACTORS"))
        {
            char *end = nullptr;
            errno = 0;
            const unsigned long count = std::strtoul(raw, &end, 10);
            if (errno != 0 || end == raw || *end != '\0' ||
                count == 0 || count > 32)
                throw std::invalid_argument(
                    "WEB_SERVER_REACTORS must be an integer in [1, 32]");
            server.setReactorCount(static_cast<size_t>(count));
        }
        // Runtime 持有可靠投递服务：状态机、最终写回执与可停止的周期重试线程共用同一生命周期。

        // ===== 中间件层：请求到达 handler 前的"安检传送带" =====

        // 请求日志中间件：像工厂门口的签到台，记录每位访客点了什么菜
        // LOG_HTTP 当前为空操作（压测纯净版），需要日志时改回真正调用
        server.router().use([operationalMetrics](RequestContext &ctx, auto next)
                            {
        operationalMetrics->recordHttpRequest();
        (void)ctx; // 压测模式下 LOG_HTTP 是空宏，仍显式标记参数已使用。
        LOG_HTTP(ctx.request.method + " " + ctx.request.path);
        next(); });

        // 统一安检：HTTP、WebSocket 握手和 SSE 握手先在这里得到可信身份。
        // 长连接接管后只复制 ctx 中已校验的 userId，不能信任客户端随意填写的 uid。
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        auto *phase11RequestAuth = phase11AuthHttp.get();
#endif
        server.router().use(
            [authTokens, allowedOrigins, operationalMetrics, rateLimiter,
             productionMode
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
             , phase11RequestAuth
#endif
             ](RequestContext &ctx, auto next)
            {
        if (!protectedPath(ctx.request.path))
        {
            next();
            return;
        }

        bool phase11Identity = false;
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        if (phase11RequestAuth &&
            (ctx.request.path == "/ws" || ctx.request.path == "/events"))
        {
            try
            {
                const auto restored = phase11RequestAuth->authorizeRequest(ctx);
                ctx.authenticated = true;
                ctx.authenticatedUserId = restored.user.id;
                ctx.authenticatedTenant = "phase11";
                phase11Identity = true;
            }
            catch (const webserver::phase11::ApplicationError &error)
            {
                if (error.code() == webserver::phase11::ErrorCode::Unavailable)
                {
                    reject(ctx, 503, "Service Unavailable",
                           "phase11 authentication is busy");
                    return;
                }
                // 若还配置了 Phase 9 Bearer Token，允许它继续作为运维/兼容入口。
            }
        }
#endif

        if (!phase11Identity && !authTokens->enabled())
        {
            // Phase 11 已启用时，/ws 与 /events 必须携带有效的 Phase 11 Session。
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
            if (phase11RequestAuth &&
                (ctx.request.path == "/ws" || ctx.request.path == "/events"))
            {
                operationalMetrics->recordAuthenticationRejected();
                reject(ctx, 401, "Unauthorized", "valid phase11 session required");
                return;
            }
#endif
            // 学习模式没有 HMAC 密钥时保留旧版 /admin 中间件契约：
            // 缺少 legacy token 仍要在路由前返回纯文本 401，防止回归测试被 404 掩盖。
            if (ctx.request.path == "/admin" &&
                ctx.request.headers.find("token") == ctx.request.headers.end())
            {
                ctx.response->status = 401;
                ctx.response->statusText = "Unauthorized";
                ctx.response->text("Unauthorized");
                return;
            }
            next();
            return;
        }

        if (!phase11Identity)
        {
            const auto authenticated = authTokens->authenticateHeaders(
                ctx.request.headers);
            if (!authenticated)
            {
                operationalMetrics->recordAuthenticationRejected();
                reject(ctx, 401, "Unauthorized", "valid bearer token required");
                return;
            }
            ctx.authenticated = true;
            ctx.authenticatedUserId = authenticated.identity->userId;
            ctx.authenticatedTenant = authenticated.identity->tenant;
        }

        if (operationsPath(ctx.request) && ctx.authenticatedTenant != "ops")
        {
            operationalMetrics->recordAuthenticationRejected();
            reject(ctx, 403, "Forbidden", "operations tenant required");
            return;
        }

        if ((ctx.request.path == "/ws" || ctx.request.path == "/events") &&
            !queryUserMatches(ctx, ctx.authenticatedUserId))
        {
            operationalMetrics->recordAuthenticationRejected();
            reject(ctx, 403, "Forbidden", "uid does not match authenticated identity");
            return;
        }

        // WebSocket 的 Origin 是浏览器替用户发来的站点身份证。生产模式必须存在；
        // 其他模式只要配置了白名单，就对携带 Origin 的浏览器请求做同样检查。
        const bool browserLongConnection =
            ctx.request.path == "/ws" || ctx.request.path == "/events";
        const bool authenticatedByCookie =
            ctx.request.headers.find("authorization") == ctx.request.headers.end() &&
            ctx.request.headers.find("cookie") != ctx.request.headers.end();
        const bool cookieStateChange =
            authenticatedByCookie && ctx.request.method != "GET" &&
            ctx.request.method != "HEAD";
        if (browserLongConnection || cookieStateChange)
        {
            const auto found = ctx.request.headers.find("origin");
            // Cookie 会被浏览器自动附带，因此任何使用 Cookie 的写请求都必须携带
            // 白名单 Origin；否则即使在开发模式，也会留下可被跨站页面利用的 CSRF 缺口。
            const bool missingRequired = (productionMode || cookieStateChange) &&
                                         found == ctx.request.headers.end();
            const bool untrusted = !allowedOrigins->empty() &&
                                   found != ctx.request.headers.end() &&
                                   !allowedOrigins->contains(found->second);
            if (missingRequired || untrusted)
            {
                operationalMetrics->recordOriginRejected();
                reject(ctx, 403, "Forbidden", "origin is not allowed");
                return;
            }
        }

        if (!rateLimiter->allow(ctx.authenticatedUserId))
        {
            operationalMetrics->recordRateLimited();
            reject(ctx, 429, "Too Many Requests", "identity request quota exceeded");
            ctx.response->setHeader("Retry-After", "60");
            return;
        }
        next(); });

        // ===== 业务路由层：每条路由就是菜单上的一道菜 =====

        // 根路径：返回一段简单 HTML，演示最基本的文本响应
        server.router().GET("/", [](RequestContext &ctx) -> bool
                            {
        ctx.response->html("<h1>hello</h1>");
        return true; }, RouteOptions{ExecutionPolicy::ReactorSafe});

        bool businessSchemaVerified=false;
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        businessSchemaVerified=static_cast<bool>(phase11Store);
#endif
        server.router().GET("/health/version", [businessSchemaVerified](RequestContext &ctx) -> bool {
            std::string version=webserver::build::versionJson;version.pop_back();
            version+=",\"schemaVerifiedAtStartup\":"+std::string(businessSchemaVerified?"true":"false")+"}";
            ctx.response->json(version);
            return true;
        });
        server.router().GET("/health/live", [](RequestContext &ctx) -> bool
                            {
        ctx.response->json("{\"status\":\"alive\"}");
        return true; }, RouteOptions{ExecutionPolicy::ReactorSafe});

        server.router().GET("/health/ready", [&server, phase11Ready](RequestContext &ctx) -> bool
                            {
        const bool ready = server.ready() && phase11Ready();
        if (!ready)
        {
            ctx.response->status = 503;
            ctx.response->statusText = "Service Unavailable";
        }
        ctx.response->json(ready
                               ? "{\"status\":\"ready\"}"
                               : "{\"status\":\"starting\"}");
        return true; });

        server.router().GET(
            "/metrics",
            [&server, operationalMetrics,
             phase11MetricsText](RequestContext &ctx) -> bool
            {
                const auto sse = server.sseManager().metrics();
                auto body = operationalMetrics->prometheus(
                    server.wsManager().onlineCount(),
                    server.sseManager().onlineCount(),
                    server.sseManager().historyEventCount());
                body += "# TYPE webserver_sse_published_total counter\n";
                body += "webserver_sse_published_total " +
                        std::to_string(sse.publishedEvents) + "\n";
                body += "webserver_sse_deliveries_total " +
                        std::to_string(sse.acceptedDeliveries) + "\n";
                body += "webserver_sse_replayed_total " +
                        std::to_string(sse.replayedEvents) + "\n";
                body += "webserver_sse_replay_gaps_total " +
                        std::to_string(sse.replayGaps) + "\n";
                body += phase11MetricsText();
                body += webserver::business_metrics::text();
                ctx.response->text(body);
                ctx.response->setHeader(
                    "Content-Type", "text/plain; version=0.0.4; charset=utf-8");
                ctx.response->setHeader("Cache-Control", "no-store");
                return true;
            });

        // 动态路由：:id 是占位符，框架自动提取 URL 实际值到 ctx.params
        server.router().GET("/user/:id", [](RequestContext &ctx) -> bool
                            {
        ctx.response->text(ctx.params.at("id"));
        return true; }, RouteOptions{ExecutionPolicy::ReactorSafe});

        // 分块传输（chunked）示例：把响应拆成多块发送，适合动态生成内容
        server.router().GET("/stream1", [](RequestContext &ctx) -> bool
                            {
        ctx.response->beginChunked();
        ctx.response->writeChunk("hello");
        ctx.response->writeChunk(" world");
        ctx.response->endChunked();
        return true; });

        // /stream2 的 sleep(1) 在 Executor Worker 线程执行，不会阻塞 Reactor
        // 这验证了"慢业务丢给线程池、主循环保持敏捷"的异步设计
        server.router().GET("/stream2", [](RequestContext &ctx) -> bool
                            {
        ctx.response->beginChunked();
        for (int i = 0; i < 10; i++)
        {
            if (ctx.stopRequested())
                break;
            ctx.response->writeChunk("hello\n");
            sleep(1);
        }
        ctx.response->endChunked();
        return true; });

        // 静态文件服务：用 sendfile 零拷贝发送 SVG 图片
        // 支持 Range 请求（断点续传），失败时返回 404
        server.router().GET("/logo", [](RequestContext &ctx) -> bool
                            {
        const std::string path = "./static/logo.svg";
        ctx.response->setHeader("Content-Type", "image/svg+xml");
        if (!ctx.response->sendfile(path, ctx.request, ctx.request.range))
        {
            ctx.response->status = 404;
            ctx.response->statusText = "Not Found";
            ctx.response->text("Not Found");
        }
        return true; });

        // Phase 11 静态入口与真实认证 API。依赖未启用时页面仍可打开，认证端点返回
        // 明确的 503，避免误把 404 理解成前端路径错误。
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        if (phase11AuthHttp)
        {
            phase11AuthHttp->registerRoutes(server.router());
            if(phase11PrivateHttp)phase11PrivateHttp->registerRoutes(server.router());
            if(phase11WebHttp)phase11WebHttp->registerRoutes(server.router());
            if(phase11Workspace)phase11Workspace->registerRoutes(server.router());
            phase11SocialChatHttp->registerRoutes(server.router());
            phase11FileHttp->registerRoutes(server.router());
            phase11KnowledgeHttp->registerRoutes(server.router());
            phase11TrainingHttp->registerRoutes(server.router());
            if (function3Runtime) function3Runtime->registerRoutes(server.router());
        }
        else
#endif
        {
            const auto unavailable = [](RequestContext &ctx) -> bool
            {
                reject(ctx, 503, "Service Unavailable",
                       "phase11 authentication is not configured");
                return true;
            };
            server.router().POST("/api/auth/register", unavailable);
            server.router().POST("/api/auth/login", unavailable);
            server.router().POST("/api/auth/logout", unavailable);
            server.router().GET("/api/me", unavailable);
            server.router().PATCH("/api/me", unavailable);
        }
        webserver::function3::registerFrontendAssets(server.router());

        // /slow: 验证 Executor 异步执行与协作式超时。
        // 像后厨每做一小步就看一次撤单灯：超过 handler deadline 后尽快停工，
        // HttpSession 再把本次结果统一改写成 504；Reactor 仍可同时服务 /fast。
        webserver::learning::registerLearningRoutes(server);

        // gRPC 使用官方 C++ 运行时和独立端口。它与 Web 服务处于同一进程，
        // 但不把自己的 HTTP/2 stream 状态塞进现有 Http2Session。
#ifdef WEBSERVER_HAS_GRPC
        std::unique_ptr<webserver::grpc_runtime::GrpcServer> grpcServer;
        const char *configuredGrpcAddress = std::getenv("WEB_GRPC_ADDRESS");
        const std::string grpcAddress = configuredGrpcAddress
                                            ? configuredGrpcAddress
                                            : "127.0.0.1:50051";
        if (grpcAddress != "off")
        {
            webserver::grpc_runtime::GrpcServerOptions grpcOptions;
            grpcOptions.address = grpcAddress;
            grpcOptions.authenticationSecret = authSecret;
            grpcOptions.metrics = operationalMetrics;
            grpcOptions.maxConcurrentRpcs = boundedEnvironmentSize(
                "WEB_GRPC_MAX_CONCURRENT_RPCS", 256, 1, 100'000);
            grpcOptions.maxWorkerThreads = boundedEnvironmentSize(
                "WEB_GRPC_MAX_WORKER_THREADS", 64, 1, 4096);
            grpcOptions.maxReceiveMessageBytes = static_cast<int>(
                boundedEnvironmentSize(
                    "WEB_GRPC_MAX_RECEIVE_BYTES", 1024 * 1024, 1024,
                    64 * 1024 * 1024));
            grpcOptions.maxSendMessageBytes = static_cast<int>(
                boundedEnvironmentSize(
                    "WEB_GRPC_MAX_SEND_BYTES", 1024 * 1024, 1024,
                    64 * 1024 * 1024));
            grpcOptions.maxRpcDuration = std::chrono::milliseconds(
                boundedEnvironmentSize(
                    "WEB_GRPC_MAX_RPC_MS", 30'000, 1,
                    24 * 60 * 60 * 1000));

            const char *grpcCert = std::getenv("WEB_GRPC_TLS_CERT");
            const char *grpcKey = std::getenv("WEB_GRPC_TLS_KEY");
            if (static_cast<bool>(grpcCert) != static_cast<bool>(grpcKey))
                throw std::invalid_argument(
                    "WEB_GRPC_TLS_CERT and WEB_GRPC_TLS_KEY must be set together");
            if (grpcCert)
            {
                grpcOptions.certificateChainPath = grpcCert;
                grpcOptions.privateKeyPath = grpcKey;
            }
            else if (productionMode)
            {
                throw std::invalid_argument(
                    "WEB_PRODUCTION_MODE requires WEB_GRPC_TLS_CERT and WEB_GRPC_TLS_KEY");
            }

            grpcServer = std::make_unique<webserver::grpc_runtime::GrpcServer>(
                std::move(grpcOptions));
            grpcServer->start();
            std::cout << "gRPC listening on " << grpcAddress
                      << " (bound port " << grpcServer->boundPort() << ")\n";
        }
#endif

        // ===== 正式开工：进入事件循环，阻塞直到收到关闭信号 =====
        server.start();
#if defined(WEBSERVER_HAS_POSTGRES) && defined(WEBSERVER_HAS_SODIUM)
        // ServerRuntime has completed its network drain. Keep repositories and event sinks
        // alive while producers cancel and accepted database/file tasks finish.
        webserver::application::BusinessShutdown businessStop;
        businessStop.add("presence",[&]{phase11PresenceBridge.reset();});
        businessStop.add("private generations",[&]{phase11PrivateGenerations.reset();phase11WebGenerations.reset();});
        businessStop.add("AI orchestration",[&]{if(phase11AiChat)phase11AiChat->shutdown();});
        businessStop.add("file tasks",[&]{if(phase11FileExecutor)phase11FileExecutor->shutdown();});
        businessStop.add("knowledge tasks",[&]{if(phase11KnowledgeExecutor)phase11KnowledgeExecutor->shutdown();});
        businessStop.add("accepted database tasks",[&]{if(phase11DatabaseExecutor)phase11DatabaseExecutor->shutdown();});
        businessStop.add("outbox",[&]{if(phase11OutboxPump)phase11OutboxPump->stop();});
        businessStop.add("community workers",[&]{if(function3Runtime)function3Runtime->stop();});
        businessStop.stop();
        if(businessStop.failures())throw std::runtime_error("business shutdown reported failures");
#endif
        return 0;
    }
    catch (const std::exception &ex)
    {
        // bind Address already in use 等：正常退出码，避免 uncaught → terminate → 核心转储。
        // 【为什么不让程序崩溃】未捕获异常会导致 std::terminate → abort → 生成 core dump，
        //   既不优雅也难排查；这里捕获后打印友好提示，返回 1 让运维知道是启动失败。
        std::cerr << "webserver failed to start: " << ex.what() << "\n"
                  << "Hint: Ctrl+Z only suspends the process and keeps port 8080; "
                     "use Ctrl+C / kill -TERM, \nor: fg then Ctrl+C, \n"
                     "or kill $(pgrep -n webserver)\n";
        return 1;
    }
}
