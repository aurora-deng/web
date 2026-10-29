#pragma once
#include "backend/ai/speech/ports/ICloudSpeech/ICloudSpeech.h"
#include <functional>
namespace webserver::function3 {
/** Linux 专用出站驱动；复用项目已有 curl + Protobuf，不下载 SDK。 */
class CurlCloudSpeech final:public nebula::speech::ICloudSpeech {
public:
    using KeyReader=std::function<std::string(nebula::speech::Provider)>;
    explicit CurlCloudSpeech(KeyReader keys):keys_(std::move(keys)){}
    bool configured(nebula::speech::Provider)const override;
    nebula::speech::CloudAudio synthesize(const nebula::speech::SpeechProfile&,const std::string&)override;
private:KeyReader keys_;
};
}
