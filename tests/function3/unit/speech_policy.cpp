#include "backend/ai/speech/application/CloudSpeechPolicy/CloudSpeechPolicy.h"
#include "backend/ai/speech/application/SpeechText/SpeechText.h"
#include "backend/ai/speech/application/WaveCodec/WaveCodec.h"
#include "backend/ai/speech/application/SpeechWire/SpeechWire.h"
#include <iostream>
using namespace nebula::speech;
void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
struct Cloud:ICloudSpeech{bool key{};int status{200},calls{};bool configured(Provider)const override{return key;}CloudAudio synthesize(const SpeechProfile&,const std::string&)override{++calls;return{status,WaveCodec::pcm24k({0,0,12,0}),{}};}};
struct Budget:ISpeechBudget{bool available{true};int calls{};bool reserve(Provider,std::size_t count)override{++calls;check(count>1024,"budget excluded request overhead");return available;}};
int main(){try{
    const auto wave=WaveCodec::pcm24k({0,0,12,0});check(WaveCodec::valid(wave),"valid PCM WAV rejected");auto corrupt=wave;corrupt[4]=0;check(!WaveCodec::valid(corrupt),"corrupt RIFF accepted");check(WaveCodec::base64("AAAMAA==")==std::vector<unsigned char>({0,0,12,0}),"PCM base64 changed bytes");bool bad=false;try{WaveCodec::base64("AA==AAAA");}catch(...){bad=true;}check(bad,"interior padding accepted");
    Cloud cloud;Budget budget;CloudSpeechPolicy policy(cloud,budget);SpeechProfile p;p.provider=Provider::Azure;int fallbacks=0;auto local=[&]{++fallbacks;return wave;};
    check(policy.generate(p,"第10名，欢迎来到星云。",local).fallbackReason=="free-tier-not-confirmed"&&cloud.calls==0,"unconfirmed free tier called provider");p.freeTierConfirmed=true;
    check(policy.generate(p,"测试",local).fallbackReason=="missing-key"&&cloud.calls==0,"missing key called provider");cloud.key=true;budget.available=false;
    check(policy.generate(p,"测试",local).fallbackReason=="application-budget-exhausted"&&cloud.calls==0,"budget overrun called provider");budget.available=true;
    check(policy.generate(p,"测试",local).provider==Provider::Azure&&cloud.calls==1,"configured cloud not used");cloud.status=429;
    check(policy.generate(p,"测试",local).fallbackReason=="provider-rate-limited","429 did not fall back");const auto calls=cloud.calls;check(policy.generate(p,"测试",local).fallbackReason=="provider-cooldown"&&calls==cloud.calls,"cooldown retried cloud");
    p.provider=Provider::Gemini;check(policy.generate(p,"测试",local).fallbackReason=="gemini-data-use-not-authorized","Gemini data consent bypassed");
    check(SpeechText::normalize("第10名 → 第2名").find("第十名")!=std::string::npos,"ranking ordinal not normalized");check(SpeechText::segments("先介绍项目。再看看新的故事！").size()==2,"semantic sentence boundaries missing");
    p.provider=Provider::Azure;auto wire=SpeechWire::request(p,"<测试 & 文案>");check(wire.host=="eastasia.tts.speech.microsoft.com"&&wire.target=="/cognitiveservices/v1"&&wire.body.find("&lt;测试 &amp; 文案&gt;")!=std::string::npos,"SSML text injection or wrong fixed target");
    p.provider=Provider::Gemini;wire=SpeechWire::request(p,"第一行\n\"第二行\"");check(wire.target=="/v1beta/models/gemini-3.1-flash-tts-preview:generateContent"&&wire.body.find("\\\"第二行\\\"")!=std::string::npos,"Gemini model or JSON escaping changed");
    check(WaveCodec::valid(SpeechWire::geminiAudio("AAAMAA==","audio/L16;codec=pcm;rate=24000")),"Gemini raw PCM not wrapped");
    check(WaveCodec::valid(SpeechWire::geminiAudio("AAAMAA==","audio/l16; rate=24000; channels=1")),"valid MIME parameters rejected");
    p.model="gemini-2.5-flash-preview-tts";check(SpeechWire::request(p,"旧版兼容").target.find(p.model)!=std::string::npos,"explicit legacy model rejected");p.model="gemini-3.1-flash-tts-preview";
    for(const auto&invalid:std::vector<std::string>{std::string("\xc0\x80",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4)}){bool rejected=false;try{SpeechText::normalize(invalid);}catch(...){rejected=true;}check(rejected,"malformed UTF8 accepted");}
    struct Throws:ISpeechBudget{bool reserve(Provider,std::size_t)override{throw std::runtime_error("offline DB");}}throws;CloudSpeechPolicy unavailable(cloud,throws);p.provider=Provider::Azure;check(unavailable.generate(p,"测试",local).fallbackReason=="budget-unavailable","budget storage failure escaped fallback");
    CloudSpeechPolicy failedLocal(cloud,budget);cloud.status=500;int attempts=0;try{failedLocal.generate(p,"测试",[&]()->std::vector<unsigned char>{++attempts;throw std::runtime_error("local offline");});}catch(...){}check(attempts==1,"failing local renderer ran twice");
    std::cout<<"PASS speech free-tier/key/consent/budget/429/fallback policy, bounded PCM/WAV and phrasing\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
