#pragma once

#include "server/phase11/ai/ModelRegistryService/ModelRegistryService.h"
#include "server/phase11/business/ChatService/ChatService.h"
#include "server/phase11/business/BusinessInterfaces/BusinessInterfaces.h"
#include "server/phase11/runtime/BoundedExecutor/BoundedExecutor.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace webserver::phase11
{

/**
 * 模型每吐出一小段文字就产生一个 token 事件。recipients 是会话成员快照，
 * 让传输层只负责投递，不需要在模型 Worker 上重新查询数据库。
 */
struct AiTokenEvent
{
    std::vector<UserId> recipients;
    std::string generationId;
    ConversationId conversationId = 0;
    std::size_t tokenIndex = 0;
    std::string text;
};

struct AiCompletedEvent
{
    std::vector<UserId> recipients;
    std::string generationId;
    ConversationId conversationId = 0;
    bool success = false;
    bool cancelled = false;
    std::string error;
    std::optional<Message> message;
};

class IAiGenerationEventSink
{
public:
    virtual ~IAiGenerationEventSink() = default;
    virtual void token(const AiTokenEvent &event) = 0;
    virtual void completed(const AiCompletedEvent &event) = 0;
};

/**
 * AI 对话的应用层编排器。
 *
 * 可以把它看成“总导演”：ChatService 负责消息落库，ModelRouter 选择模型演员，
 * EventSink 把拍摄中的片段实时送到浏览器。所有数据库工作进入专用执行器，调用
 * generate() 的 WebSocket Worker 只做校验和入队，不等待 PostgreSQL 或模型。
 */
class AiChatService final : public IAiChatBusiness
{
public:
    // 只公开不完整类型，便于 .cpp 中的生命周期辅助函数共享状态；调用方无法访问内容。
    struct State;

    AiChatService(ChatService &chat,
                  IUserRepository &users,
                  IConversationRepository &conversations,
                  ModelRouter &models,
                  DatabaseExecutor &databaseExecutor,
                  IAiGenerationEventSink &events);
    ~AiChatService();

    AiChatService(const AiChatService &) = delete;
    AiChatService &operator=(const AiChatService &) = delete;

    /** 接纳请求后立即返回；验证、落库和推理在受控后台线程中继续。 */
    void generate(AiChatRequest request) override;

    /** 只有原请求用户能取消。false 表示请求不存在或不属于该用户。 */
    [[nodiscard]] bool cancel(UserId requester,
                              const std::string &generationId) noexcept override;

    /** 取消在途生成并等待其唯一完成回调收尾。可重复调用。 */
    void shutdown() noexcept;

private:
    std::shared_ptr<State> state_;
};

} // namespace webserver::phase11
