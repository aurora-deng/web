#pragma once
#include <memory>
#include <string>
#include <functional>
#include <cstdint>
class Router;
class SseSessionManager;
class WebSocketSessionManager;
namespace webserver::phase11 { class BoundedExecutor; namespace transport {class Phase11AuthHttp;} }
namespace webserver::phase11 {class IOutboxEventSink;class IMessageRepository;class IFileBusiness;class ISocialBusiness;}
namespace webserver::phase11 {class IModelRepository;class ModelRegistryService;class ModelEvaluationService;}
namespace webserver::function3 {
/** 装配根：只连接各模块接口，排名线程和驱动在内部拥有明确生命周期。 */
class Function3Runtime final {
public:
    Function3Runtime(std::string connection,phase11::transport::Phase11AuthHttp &auth,
        phase11::BoundedExecutor &executor,SseSessionManager &sse,phase11::IFileBusiness &files,phase11::ISocialBusiness&social,WebSocketSessionManager &websockets,
        phase11::IModelRepository&models,phase11::ModelRegistryService&registry,phase11::ModelEvaluationService&evaluation,std::function<void(std::uint64_t)>invalidatePrivateKnowledge={});
    ~Function3Runtime();
    Function3Runtime(const Function3Runtime&)=delete;
    Function3Runtime&operator=(const Function3Runtime&)=delete;
    void registerRoutes(Router &router);
    phase11::IOutboxEventSink &startMessaging(phase11::IOutboxEventSink &realtime,phase11::IMessageRepository &messages);
    /** 仅捕获独立通知状态，允许存储组件比 Runtime 更晚析构。 */
    std::function<void()> commitNotifier()const;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
