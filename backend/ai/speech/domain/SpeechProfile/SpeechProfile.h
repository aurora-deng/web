#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace nebula::speech {
enum class Provider{Local,Azure,Gemini};
struct SpeechProfile{
    Provider provider{Provider::Local};std::string voice{"zf_001"},style{"calm"},region{"eastasia"};
    double speed{.95};int pitchPercent{-2};bool freeTierConfirmed{},dataUseConsent{};
    std::string model{"gemini-3.1-flash-tts-preview"};
};
struct CloudAudio{int status{};std::vector<unsigned char>wave;std::string error;};
struct SpeechResult{std::vector<unsigned char>wave;Provider provider{Provider::Local};std::string fallbackReason;};
inline std::string providerName(Provider p){return p==Provider::Azure?"azure":p==Provider::Gemini?"gemini":"local";}
struct CloudRequest{
    std::string host,target,contentType,body,voiceName;
    Provider provider{};
};
}
