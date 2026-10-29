#pragma once
#include "backend/ai/speech/ports/ICloudSpeech/ICloudSpeech.h"
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
namespace nebula::speech {
/** 无Key/非免费配置/拒绝数据用途/预算不足/429/超时均回退，不阻断文字业务。 */
class CloudSpeechPolicy final{
public:
    CloudSpeechPolicy(ICloudSpeech&cloud,ISpeechBudget&budget):cloud_(cloud),budget_(budget){}
    SpeechResult generate(SpeechProfile profile,std::string text,const std::function<std::vector<unsigned char>()>&local);
    static void validate(const SpeechProfile&profile);
private:ICloudSpeech&cloud_;ISpeechBudget&budget_;std::mutex mutex_;std::map<Provider,std::chrono::steady_clock::time_point>blockedUntil_;
};
}
