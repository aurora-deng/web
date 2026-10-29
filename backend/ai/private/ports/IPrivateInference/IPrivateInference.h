#pragma once
#include "backend/ai/models/ModelProvider/ModelProvider.h"
namespace webserver::phase11 {
/** 私人推理端口只有模型输入/回调；故意不提供聊天、训练或审计正文写入接口。 */
class IPrivateInference {
public:
    virtual ~IPrivateInference()=default;
    virtual GenerationHandle generate(UserId ai,GenerationRequest request,TokenSink token,
        CompletionSink complete,CancellationToken cancellation)=0;
};
}
