#include "server/phase11/ai/DeterministicKnowledgeIndex/DeterministicKnowledgeIndex.h"
#include "server/phase11/ai/RagService/RagService.h"
#include "server/phase11/storage/InMemoryStore/InMemoryStore.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace webserver::phase11;

#define CHECK(expression)                                                       \
    do                                                                          \
    {                                                                           \
        if (!(expression))                                                      \
        {                                                                       \
            std::cerr << "CHECK failed: " #expression << " at " << __FILE__    \
                      << ':' << __LINE__ << '\n';                               \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

namespace
{

class RejectingIndex final : public IKnowledgeIndex
{
public:
    void replaceAll(std::vector<KnowledgeChunk>) override {}
    void upsert(std::vector<KnowledgeChunk>) override
    {
        throw std::runtime_error("index offline");
    }
    void removeDocument(KnowledgeDocumentId) override { removed = true; }
    std::vector<KnowledgeSearchHit> search(
        std::string_view, const std::vector<KnowledgeDocumentId> &,
        std::size_t) const override { return {}; }
    bool removed = false;
};

bool throwsCode(const std::function<void()> &operation, ErrorCode expected)
{
    try { operation(); }
    catch (const ApplicationError &error) { return error.code() == expected; }
    return false;
}

} // namespace

int main()
{
    const auto now = std::chrono::system_clock::now();
    InMemoryStore store;
    auto alice = store.create(
        {0, "rag-alice", "Alice", {}, "hash", false, false, now});
    auto bob = store.create(
        {0, "rag-bob", "Bob", {}, "hash", false, false, now});
    auto outsider = store.create(
        {0, "rag-outsider", "Outsider", {}, "hash", false, false, now});
    auto ai = store.create(
        {0, "rag-ai", "RAG AI", {}, "hash", false, true, now});
    auto conversation = store.createConversation(
        {0, ConversationKind::Group, "RAG Study", alice.id, 1, now},
        {{0, alice.id, MemberRole::Owner, 0, 0, now},
         {0, bob.id, MemberRole::Member, 0, 0, now}});

    RagServiceConfig config;
    config.maxChunkBytes = 256;
    config.overlapBytes = 32;
    config.maxRetrievedChunks = 6;
    DeterministicKnowledgeIndex index;
    RagService rag(store, store, store, index, config);

    const auto privateDocument = rag.registerDocument(
        alice.id, {}, "Alice 私人笔记", "alice.md", "text/markdown",
        "HTTP/2 流控通过连接窗口和 Stream 窗口共同限制发送量。", now);
    const auto sharedDocument = rag.registerDocument(
        bob.id, conversation.id, "群共享架构", "architecture.md",
        "text/markdown",
        "RAG 检索必须先做成员权限过滤，再把引用片段交给模型。", now);
    const auto bobPrivate = rag.registerDocument(
        bob.id, {}, "Bob 私密", "secret.txt", "text/plain",
        "火星口令只属于 Bob 私人资料，不应该被 Alice 检索。", now);
    CHECK(privateDocument.state == KnowledgeDocumentState::Ready);
    CHECK(sharedDocument.state == KnowledgeDocumentState::Ready);
    CHECK(bobPrivate.state == KnowledgeDocumentState::Ready);

    const auto context = rag.retrieveForGeneration(
        "rag-generation-1", alice.id, conversation.id,
        "成员权限过滤和引用片段", 6, now);
    CHECK(!context.citations.empty());
    CHECK(context.systemPrompt.find("群共享架构") != std::string::npos);
    for (const auto &citation : context.citations)
    {
        CHECK(citation.documentId != bobPrivate.id);
        CHECK(citation.documentId != privateDocument.id);
    }
    CHECK(store.findRagRetrieval("rag-generation-1").has_value());
    CHECK(store.ragCitations("rag-generation-1") == context.citations);

    // 同一个 generationId 重放同一命令会复用已经持久化的引用，不重复写入。
    const auto replay = rag.retrieveForGeneration(
        "rag-generation-1", alice.id, conversation.id,
        "成员权限过滤和引用片段", 6, now);
    CHECK(replay.citations.size() == context.citations.size());
    CHECK(throwsCode([&] {
        (void)rag.retrieveForGeneration(
            "rag-generation-1", alice.id, conversation.id,
            "另一个查询", 6, now);
    }, ErrorCode::Conflict));

    CHECK(throwsCode([&] {
        (void)rag.retrieveForGeneration(
            "rag-outsider", outsider.id, conversation.id,
            "权限", 6, now);
    }, ErrorCode::Forbidden));
    CHECK(throwsCode([&] {
        (void)rag.citations(outsider.id, "rag-generation-1");
    }, ErrorCode::Forbidden));

    // 派生索引可以在进程重启后完全由 Ready Chunk 重建。
    DeterministicKnowledgeIndex rebuiltIndex;
    RagService afterRestart(store, store, store, rebuiltIndex, config);
    const auto direct = store.createConversation(
        {0, ConversationKind::Direct, "Alice AI", alice.id, 1, now},
        {{0, alice.id, MemberRole::Member, 0, 0, now},
         {0, ai.id, MemberRole::Member, 0, 0, now}});
    const auto rebuilt = afterRestart.retrieveForGeneration(
        "rag-generation-2", alice.id, direct.id,
        "HTTP 连接 Stream 流控窗口", 6, now);
    CHECK(!rebuilt.citations.empty());
    CHECK(rebuilt.citations.front().documentId == privateDocument.id);

    // 索引失败时文档不能伪装成 Ready，失败原因留在登记卡中。
    RejectingIndex rejecting;
    RagService unavailable(store, store, store, rejecting, config);
    try
    {
        (void)unavailable.registerDocument(
            alice.id, {}, "失败样例", "failed.txt", "text/plain",
            "这段正文会在索引阶段失败。", now);
        CHECK(false);
    }
    catch (const ApplicationError &error)
    {
        CHECK(error.code() == ErrorCode::Unavailable);
    }
    CHECK(rejecting.removed);
    // Ready 列表不会暴露失败文档；从连续 ID 可以找到并检查失败状态。
    const auto failed = store.findKnowledgeDocument(bobPrivate.id + 1);
    CHECK(failed && failed->state == KnowledgeDocumentState::Failed &&
          !failed->failureDetail.empty());
    std::cout << "phase11 RAG service tests passed\n";
    return 0;
}
