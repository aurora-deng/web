#include "PrivateModelRouter.h"
namespace webserver::phase11 {
GenerationHandle PrivateModelRouter::generate(UserId ai,GenerationRequest request,TokenSink token,
    CompletionSink complete,CancellationToken cancellation) {
    const auto user=users_.findUser(ai);
    if(!user||!user->aiAccount||user->disabled)throw ApplicationError(ErrorCode::NotFound,"private model account unavailable");
    // onSelected 在 Provider 接收正文之前调用。私人接口不允许自动走云/Ollama备用地址。
    return models_.generate(ai,std::move(request),std::move(token),std::move(complete),std::move(cancellation),
        [](const GenerationSelection&selection){if(selection.runtime!="onnx-inprocess"&&selection.runtime!="onnx-grpc")
            throw ApplicationError(ErrorCode::Forbidden,"private inference requires a configured self-hosted ONNX provider");});
}
}
