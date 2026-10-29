#pragma once
#include "backend/ai/speech/domain/SpeechProfile/SpeechProfile.h"
#include <string>
namespace webserver::function3 {
class ISpeechControl {
public:
    virtual ~ISpeechControl()=default;
    virtual nebula::speech::SpeechProfile profile(const std::string&voice)const=0;
    virtual void configure(const nebula::speech::SpeechProfile&,unsigned long long actor)=0;
    virtual std::string status()const=0;
};
}
