#pragma once
#include "backend/ai/private/ports/IPrivateInference/IPrivateInference.h"
#include <chrono>
#include <memory>
namespace webserver::phase11 {
struct PrivateGenerationConfig {
    std::size_t activeLimit{8},recordsLimit{128},responseBytesLimit{65536};
    std::chrono::milliseconds lease{30000},timeout{180000},retention{300000};
};
struct PrivateGenerationView {
    std::string id,state,error;std::vector<std::string>pieces;
    std::size_t next{},bytes{};std::uint64_t firstTokenMicroseconds{};
    MessageModelTrace trace;
};
/** 仅RAM中的有界租约任务。没有 Message/Outbox/训练/正文日志写入依赖。 */
class PrivateGenerationService final {
public:
    PrivateGenerationService(IPrivateInference&model,PrivateGenerationConfig config={});
    ~PrivateGenerationService();
    PrivateGenerationService(const PrivateGenerationService&)=delete;
    PrivateGenerationService&operator=(const PrivateGenerationService&)=delete;
    PrivateGenerationView start(UserId owner,UserId ai,GenerationRequest request);
    PrivateGenerationView poll(UserId owner,const std::string&id,std::size_t after);
    bool cancel(UserId owner,const std::string&id);
private:struct State;std::shared_ptr<State>state_;
};
}
