#include "server/grpc/GrpcServer.h"

#include "server/ops/OperationalMetrics.h"
#include "server/security/AuthToken.h"
#include "web_learning.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr char kSecret[] = "phase9-grpc-integration-secret-32-bytes";

std::string readFile(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::string issueToken()
{
    using namespace std::chrono;
    webserver::security::AuthToken issuer(kSecret);
    const auto expiry = duration_cast<seconds>(
                            system_clock::now().time_since_epoch())
                            .count() +
                        3600;
    return issuer.issue({42, "learning", static_cast<std::uint64_t>(expiry)});
}

void authenticate(::grpc::ClientContext &context,
                  const std::string &token,
                  std::string requestId)
{
    context.AddMetadata("authorization", "Bearer " + token);
    context.AddMetadata("x-request-id", std::move(requestId));
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(5));
}

void verifyRpcRoundTrip(
    webserver::grpc_runtime::GrpcServerOptions options,
    std::shared_ptr<::grpc::ChannelCredentials> credentials,
    const std::string &host)
{
    const auto metrics = std::make_shared<webserver::ops::OperationalMetrics>();
    options.authenticationSecret = kSecret;
    options.maxRpcDuration = std::chrono::seconds(2);
    options.maxConcurrentRpcs = 2;
    // 把 Core 线程预算压到 1，专门验证 Callback Reactor 在 Alarm 等待期间
    // 不会像同步 handler 的 sleep 那样独占一条业务工作线程。
    options.maxWorkerThreads = 1;
    options.metrics = metrics;

    webserver::grpc_runtime::GrpcServer server(std::move(options));
    server.start();
    assert(server.running() && server.boundPort() > 0);

    const auto endpoint = host + ":" + std::to_string(server.boundPort());
    auto channel = ::grpc::CreateChannel(endpoint, std::move(credentials));
    auto stub = ::webtest::rpc::v1::LearningService::NewStub(channel);
    const auto token = issueToken();

    // 所有 RPC 先走统一认证；缺少 metadata 时返回 gRPC status，而非断开连接。
    ::grpc::ClientContext unauthenticatedContext;
    ::webtest::rpc::v1::EchoRequest unauthenticatedRequest;
    ::webtest::rpc::v1::EchoReply unauthenticatedReply;
    unauthenticatedRequest.set_message("blocked");
    const auto unauthenticated = stub->Echo(
        &unauthenticatedContext, unauthenticatedRequest, &unauthenticatedReply);
    assert(unauthenticated.error_code() == ::grpc::StatusCode::UNAUTHENTICATED);

    // Unary：验证生成代码、认证身份、request-id 与 Protobuf/HTTP2 往返。
    ::grpc::ClientContext echoContext;
    authenticate(echoContext, token, "integration-echo");
    ::webtest::rpc::v1::EchoRequest echoRequest;
    ::webtest::rpc::v1::EchoReply echoReply;
    echoRequest.set_message("phase9");
    const auto echoStatus = stub->Echo(&echoContext, echoRequest, &echoReply);
    assert(echoStatus.ok());
    assert(echoReply.message() == "phase9");
    assert(echoReply.server_sequence() == 1);
    const auto initial = echoContext.GetServerInitialMetadata();
    assert(initial.find("x-request-id") != initial.end());
    assert(initial.find("x-authenticated-user") != initial.end());

    // Server streaming：同一个 stream 中顺序返回多条消息。
    ::grpc::ClientContext countContext;
    authenticate(countContext, token, "integration-count");
    ::webtest::rpc::v1::CountRequest countRequest;
    countRequest.set_limit(3);
    auto reader = stub->Count(&countContext, countRequest);
    ::webtest::rpc::v1::CountReply countReply;
    std::vector<std::uint32_t> values;
    while (reader->Read(&countReply))
        values.push_back(countReply.value());
    assert(reader->Finish().ok());
    assert((values == std::vector<std::uint32_t>{1, 2, 3}));

    // Client streaming：客户端连续写块，服务端在半关闭后返回汇总。
    ::grpc::ClientContext uploadContext;
    authenticate(uploadContext, token, "integration-upload");
    ::webtest::rpc::v1::UploadSummary uploadSummary;
    auto upload = stub->Upload(&uploadContext, &uploadSummary);
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
    {
        ::webtest::rpc::v1::UploadChunk chunk;
        chunk.set_sequence(sequence);
        chunk.set_payload(std::string(static_cast<std::size_t>(sequence), 'x'));
        assert(upload->Write(chunk));
    }
    upload->WritesDone();
    assert(upload->Finish().ok());
    assert(uploadSummary.chunk_count() == 3);
    assert(uploadSummary.byte_count() == 6);
    assert(uploadSummary.last_sequence() == 3);

    // Bidirectional streaming：每条输入独立映射为一条输出，stream 无需等待整批结束。
    ::grpc::ClientContext chatContext;
    authenticate(chatContext, token, "integration-chat");
    auto chat = stub->Chat(&chatContext);
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence)
    {
        ::webtest::rpc::v1::ChatMessage outbound;
        outbound.set_sequence(sequence);
        outbound.set_text("message-" + std::to_string(sequence));
        assert(chat->Write(outbound));
        ::webtest::rpc::v1::ChatMessage inbound;
        assert(chat->Read(&inbound));
        assert(inbound.sequence() == sequence);
        assert(inbound.text() == "echo:" + outbound.text());
    }
    chat->WritesDone();
    ::webtest::rpc::v1::ChatMessage noMoreMessages;
    assert(!chat->Read(&noMoreMessages));
    assert(chat->Finish().ok());

    // 参数错误只结束当前 RPC，不伤害共享的 HTTP/2 connection。
    ::grpc::ClientContext invalidContext;
    authenticate(invalidContext, token, "integration-invalid");
    ::webtest::rpc::v1::CountRequest invalidRequest;
    invalidRequest.set_limit(0);
    auto invalidReader = stub->Count(&invalidContext, invalidRequest);
    assert(!invalidReader->Read(&countReply));
    assert(invalidReader->Finish().error_code() ==
           ::grpc::StatusCode::INVALID_ARGUMENT);

    // deadline 到期后服务端循环会在最多约 10ms 内看见取消并停止工作。
    ::grpc::ClientContext deadlineContext;
    deadlineContext.AddMetadata("authorization", "Bearer " + token);
    deadlineContext.set_deadline(std::chrono::system_clock::now() +
                                 std::chrono::milliseconds(40));
    ::webtest::rpc::v1::CountRequest slowRequest;
    slowRequest.set_limit(100);
    slowRequest.set_interval_ms(50);
    auto slowReader = stub->Count(&deadlineContext, slowRequest);
    while (slowReader->Read(&countReply))
    {
    }
    const auto deadlineStatus = slowReader->Finish();
    assert(deadlineStatus.error_code() == ::grpc::StatusCode::DEADLINE_EXCEEDED ||
           deadlineStatus.error_code() == ::grpc::StatusCode::CANCELLED);

    // 客户端收到 deadline 与 Callback Reactor 执行 OnDone 不是同一个瞬间。
    // 留出一个小的有界窗口，让取消回调完成并由 CallbackCall 归还业务配额，
    // 避免下一段并发测试把“上一调用正在收尾”误判成实现错误。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 第一个慢流写完首条消息后进入 1 秒 Alarm。Callback Reactor 此时已把
    // 执行线程还给运行时，所以同一服务上的 unary RPC 应快速完成。
    ::grpc::ClientContext firstOccupiedContext;
    authenticate(firstOccupiedContext, token, "integration-callback-wait");
    ::webtest::rpc::v1::CountRequest occupiedRequest;
    occupiedRequest.set_limit(10);
    occupiedRequest.set_interval_ms(1000);
    auto firstOccupiedReader = stub->Count(
        &firstOccupiedContext, occupiedRequest);
    assert(firstOccupiedReader->Read(&countReply));

    ::grpc::ClientContext concurrentEchoContext;
    authenticate(concurrentEchoContext, token, "integration-callback-echo");
    ::webtest::rpc::v1::EchoRequest concurrentEchoRequest;
    ::webtest::rpc::v1::EchoReply concurrentEchoReply;
    concurrentEchoRequest.set_message("callback-does-not-block");
    const auto concurrentStarted = std::chrono::steady_clock::now();
    const auto concurrentEchoStatus = stub->Echo(
        &concurrentEchoContext, concurrentEchoRequest, &concurrentEchoReply);
    const auto concurrentElapsed = std::chrono::steady_clock::now() -
                                   concurrentStarted;
    assert(concurrentEchoStatus.ok());
    assert(concurrentEchoReply.message() == "callback-does-not-block");
    assert(concurrentElapsed < std::chrono::milliseconds(500));

    // 再占用第二个业务准入名额。第三个 RPC 必须快速收到配额拒绝，说明
    // Callback 改造没有绕过原有的 active-RPC 内存与业务容量边界。
    ::grpc::ClientContext secondOccupiedContext;
    authenticate(secondOccupiedContext, token, "integration-quota-owner-2");
    auto secondOccupiedReader = stub->Count(
        &secondOccupiedContext, occupiedRequest);
    assert(secondOccupiedReader->Read(&countReply));

    ::grpc::ClientContext rejectedContext;
    authenticate(rejectedContext, token, "integration-quota-rejected");
    ::webtest::rpc::v1::EchoRequest rejectedRequest;
    ::webtest::rpc::v1::EchoReply rejectedReply;
    rejectedRequest.set_message("over quota");
    const auto rejectedStatus = stub->Echo(
        &rejectedContext, rejectedRequest, &rejectedReply);
    assert(rejectedStatus.error_code() == ::grpc::StatusCode::RESOURCE_EXHAUSTED);
    firstOccupiedContext.TryCancel();
    secondOccupiedContext.TryCancel();
    while (firstOccupiedReader->Read(&countReply))
    {
    }
    while (secondOccupiedReader->Read(&countReply))
    {
    }
    const auto firstOccupiedStatus = firstOccupiedReader->Finish();
    const auto secondOccupiedStatus = secondOccupiedReader->Finish();
    assert(firstOccupiedStatus.error_code() == ::grpc::StatusCode::CANCELLED ||
           firstOccupiedStatus.error_code() ==
               ::grpc::StatusCode::DEADLINE_EXCEEDED);
    assert(secondOccupiedStatus.error_code() == ::grpc::StatusCode::CANCELLED ||
           secondOccupiedStatus.error_code() ==
               ::grpc::StatusCode::DEADLINE_EXCEEDED);

    server.stop(std::chrono::milliseconds(500));
    assert(!server.running());
    assert(metrics->grpcRejected.load() >= 1);
    assert(metrics->grpcStarted.load() >= 6);
}
} // namespace

int main()
{
    webserver::grpc_runtime::GrpcServerOptions insecure;
    insecure.address = "127.0.0.1:0";
    verifyRpcRoundTrip(
        std::move(insecure), ::grpc::InsecureChannelCredentials(), "127.0.0.1");

    // 设置这两个变量时，再跑一遍真实 TLS + ALPN(h2) gRPC 往返。
    const char *certificate = std::getenv("WEB_GRPC_TEST_CERT");
    const char *privateKey = std::getenv("WEB_GRPC_TEST_KEY");
    assert(static_cast<bool>(certificate) == static_cast<bool>(privateKey));
    if (certificate)
    {
        webserver::grpc_runtime::GrpcServerOptions secure;
        secure.address = "127.0.0.1:0";
        secure.certificateChainPath = certificate;
        secure.privateKeyPath = privateKey;

        ::grpc::SslCredentialsOptions clientTls;
        clientTls.pem_root_certs = readFile(certificate);
        verifyRpcRoundTrip(
            std::move(secure), ::grpc::SslCredentials(clientTls), "localhost");
    }
    return 0;
}
