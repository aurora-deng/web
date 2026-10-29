#pragma once
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
#include "backend/ai/speech/application/SpeechService/SpeechService.h"
#include <map>
#include <memory>
#include <mutex>
namespace webserver::function3 {
class SpeechAdminHttp final {
public:
    SpeechAdminHttp(phase11::transport::Phase11AuthHttp&auth,phase11::BoundedExecutor&executor,IActorDirectory&actors,SpeechService&speech):auth_(auth),executor_(executor),actors_(actors),speech_(speech){}
    void registerRoutes(Router&router);
private:
    struct Preview { std::string state="pending",key;std::uint64_t revision{};double speed{}; };
    struct State { std::mutex mutex;std::map<std::string,Preview> voices;std::uint64_t revision{}; };
    std::shared_ptr<State> state_=std::make_shared<State>(); // 固定两个音色，状态缓存有界。
    std::string previewState(const std::string&voice,bool start,bool personal=false,double speed=0);
    phase11::transport::Phase11AuthHttp&auth_;phase11::BoundedExecutor&executor_;IActorDirectory&actors_;SpeechService&speech_;
};
}
