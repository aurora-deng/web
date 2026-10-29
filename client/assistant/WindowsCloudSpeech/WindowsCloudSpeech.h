#pragma once
#include "backend/ai/speech/ports/ICloudSpeech/ICloudSpeech.h"
#include <functional>
namespace nebula::client {
/** 仅对固定云端 HTTPS 目标发起出站请求，使用 Windows 系统证书验证。 */
class WindowsCloudSpeech final:public speech::ICloudSpeech {
public:
    using KeyReader=std::function<std::string(speech::Provider)>;
    explicit WindowsCloudSpeech(KeyReader reader):keys_(std::move(reader)){}
    bool configured(speech::Provider)const override;
    speech::CloudAudio synthesize(const speech::SpeechProfile&,const std::string&)override;
private:KeyReader keys_;
};
}
