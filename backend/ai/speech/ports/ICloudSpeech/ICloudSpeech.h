#pragma once
#include "backend/ai/speech/domain/SpeechProfile/SpeechProfile.h"
namespace nebula::speech {
class ICloudSpeech{
public:virtual~ICloudSpeech()=default;virtual bool configured(Provider provider)const=0;
    virtual CloudAudio synthesize(const SpeechProfile&profile,const std::string&text)=0;
};
class ISpeechBudget{
public:virtual~ISpeechBudget()=default;
    // 持久预留尝试预算；失败是否实际计费不明确时不回退，避免把额度算得过于乐观。
    virtual bool reserve(Provider provider,std::size_t upperBound)=0;
};
}
