#include "CloudSpeechPolicy.h"
#include "backend/ai/speech/application/SpeechText/SpeechText.h"
#include "backend/ai/speech/application/WaveCodec/WaveCodec.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace nebula::speech {
void CloudSpeechPolicy::validate(const SpeechProfile&p){
    if(p.provider!=Provider::Local&&p.provider!=Provider::Azure&&p.provider!=Provider::Gemini)throw std::invalid_argument("unsupported speech provider");
    if(p.voice!="zf_001"&&p.voice!="zm_009")throw std::invalid_argument("unsupported speech voice alias");
    if(!std::isfinite(p.speed)||p.speed<.75||p.speed>1.25||p.pitchPercent<-6||p.pitchPercent>3)throw std::invalid_argument("speech prosody exceeds range");
    if(p.region.empty()||p.region.size()>32||p.region.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-")!=std::string::npos)throw std::invalid_argument("invalid Azure region");
    if(p.model!="gemini-2.5-flash-preview-tts")throw std::invalid_argument("only the explicitly free-configured Gemini model is allowed");
    const bool male=p.voice=="zm_009";
    if(p.style!="calm"&&p.style!="gentle"&&p.style!="serious"&&p.style!="chat"&&p.style!="narration-relaxed")throw std::invalid_argument("unsupported voice style");
    if(p.provider==Provider::Azure&&((male&&(p.style=="calm"||p.style=="gentle"))||(!male&&(p.style=="chat"||p.style=="narration-relaxed"))))throw std::invalid_argument("this Azure voice does not support the selected style");
}
SpeechResult CloudSpeechPolicy::generate(SpeechProfile p,std::string text,const std::function<std::vector<unsigned char>()>&local){
    validate(p);text=SpeechText::normalize(std::move(text));auto fallback=[&](std::string reason){return SpeechResult{local(),Provider::Local,std::move(reason)};};
    if(p.provider==Provider::Local)return fallback("");if(!p.freeTierConfirmed)return fallback("free-tier-not-confirmed");
    if(p.provider==Provider::Gemini&&!p.dataUseConsent)return fallback("gemini-data-use-not-authorized");
    if(!cloud_.configured(p.provider))return fallback("missing-key");
    bool cooling;{std::lock_guard lock(mutex_);cooling=blockedUntil_[p.provider]>std::chrono::steady_clock::now();}if(cooling)return fallback("provider-cooldown");
    bool budgetAllowed=false;try{budgetAllowed=budget_.reserve(p.provider,text.size()+1024);}catch(...){return fallback("budget-unavailable");}
    if(!budgetAllowed)return fallback("application-budget-exhausted");
    CloudAudio result;try{result=cloud_.synthesize(p,text);}catch(...){return fallback("provider-network-or-timeout");}
    if(result.status==200&&WaveCodec::valid(result.wave))return {std::move(result.wave),p.provider,{}};
        if(result.status==429){std::lock_guard lock(mutex_);blockedUntil_[p.provider]=std::chrono::steady_clock::now()+std::chrono::seconds(60);}
        return fallback(result.status==429?"provider-rate-limited":result.status==401||result.status==403?"provider-credentials-rejected":result.status==0?"provider-network-or-timeout":"provider-invalid-response");
}
}
