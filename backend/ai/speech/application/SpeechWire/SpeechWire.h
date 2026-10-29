#pragma once
#include "backend/ai/speech/domain/SpeechProfile/SpeechProfile.h"
namespace nebula::speech {
/** 只负责协议载荷；密钥由平台驱动加入请求头，不能进入 URL、日志或业务对象。 */
class SpeechWire final {
public:
    static CloudRequest request(const SpeechProfile&,const std::string&normalizedText);
    static std::vector<unsigned char> geminiAudio(const std::string&base64,const std::string&mime);
};
}
