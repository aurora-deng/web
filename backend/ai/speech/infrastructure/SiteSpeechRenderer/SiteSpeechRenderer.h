#pragma once
#include "backend/ai/speech/infrastructure/SpeechProcessRenderer/SpeechProcessRenderer.h"
#include "backend/ai/speech/infrastructure/CurlCloudSpeech/CurlCloudSpeech.h"
#include "backend/ai/speech/application/CloudSpeechPolicy/CloudSpeechPolicy.h"
#include "backend/ai/speech/ports/ISpeechControl/ISpeechControl.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
/** 公告云优先、本地兜底。此驱动只在已有 curl/Protobuf 可用时编译。 */
class SiteSpeechRenderer final:public ISpeechRenderer,public ISpeechControl,private nebula::speech::ISpeechBudget {
public:
    SiteSpeechRenderer(SqlContext&,SpeechProcessRenderer&);
    std::string render(const SpeechJob&)override;
    std::string path(std::string key)const override{return local_.path(std::move(key));}
    nebula::speech::SpeechProfile profile(const std::string&)const override;
    void configure(const nebula::speech::SpeechProfile&,unsigned long long actor)override;
    void configureCredential(nebula::speech::Provider,const std::string&,bool,unsigned long long actor)override;
    std::string status()const override;
private:
    bool reserve(nebula::speech::Provider,std::size_t)override;
    static std::string key(nebula::speech::Provider);
    SqlContext&sql_;SpeechProcessRenderer&local_;CurlCloudSpeech cloud_;nebula::speech::CloudSpeechPolicy policy_;
    mutable std::mutex mutex_;std::mutex rendering_;std::string lastProvider_="local",lastFallback_;
};
}
