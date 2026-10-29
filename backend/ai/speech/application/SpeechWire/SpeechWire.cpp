#include "SpeechWire.h"
#include "backend/ai/speech/application/CloudSpeechPolicy/CloudSpeechPolicy.h"
#include "backend/ai/speech/application/WaveCodec/WaveCodec.h"
#include <stdexcept>
namespace nebula::speech {
namespace {
std::string xml(const std::string&s){std::string out;for(char c:s){switch(c){case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '\"':out+="&quot;";break;case '\'':out+="&apos;";break;default:out+=c;}}return out;}
std::string json(const std::string&s){std::string out="\"";for(unsigned char c:s){if(c=='\"'||c=='\\'){out+='\\';out+=c;}else if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else if(c=='\t')out+="\\t";else if(c<32)throw std::invalid_argument("invalid speech control byte");else out+=c;}return out+'\"';}
}
CloudRequest SpeechWire::request(const SpeechProfile&p,const std::string&text){
    CloudSpeechPolicy::validate(p);if(text.empty()||text.size()>8192)throw std::invalid_argument("speech wire text too large");
    CloudRequest r;r.provider=p.provider;const bool male=p.voice=="zm_009";
    if(p.provider==Provider::Azure){r.host=p.region+".tts.speech.microsoft.com";r.target="/cognitiveservices/v1";r.contentType="application/ssml+xml";r.voiceName=male?"zh-CN-YunxiNeural":"zh-CN-XiaomoNeural";
        const auto rate=static_cast<int>((p.speed-1)*100);const auto signedValue=[](int v){return (v>=0?"+":"")+std::to_string(v)+"%";};
        r.body="<speak version='1.0' xml:lang='zh-CN' xmlns='http://www.w3.org/2001/10/synthesis' xmlns:mstts='https://www.w3.org/2001/mstts'><voice name='"+r.voiceName+"'><mstts:express-as style='"+p.style+"' role='"+(male?"YoungAdultMale":"OlderAdultFemale")+"'><prosody rate='"+signedValue(rate)+"' pitch='"+signedValue(p.pitchPercent)+"'>"+xml(text)+"</prosody></mstts:express-as></voice></speak>";
    }else if(p.provider==Provider::Gemini){r.host="generativelanguage.googleapis.com";r.target="/v1beta/models/"+p.model+":generateContent";r.contentType="application/json";r.voiceName=male?"Charon":"Kore";
        const std::string direction=male?"请用自然、低沉磁性而保留青年感的中文男声朗读，适当停顿，不要播音腔。":"请用自然、沉稳成熟的中文女声朗读，适当停顿，不要机械播报。";
        r.body="{\"contents\":[{\"parts\":[{\"text\":"+json(direction+"语速比例 "+std::to_string(p.speed)+"。只朗读以下内容：\n"+text)+"}]}],\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":"+json(r.voiceName)+"}}}}}";
    }else throw std::invalid_argument("local speech has no cloud request");return r;
}
std::vector<unsigned char>SpeechWire::geminiAudio(const std::string&data,const std::string&mime){
    // 固定 2.5 TTS 的原始 PCM 契约；拒绝未知采样率或格式，防止把错误 JSON 当 WAV 播放。
    if(mime!="audio/L16;codec=pcm;rate=24000"&&mime!="audio/pcm;rate=24000"&&mime!="audio/L16;rate=24000")throw std::invalid_argument("unsupported Gemini audio format");
    return WaveCodec::pcm24k(WaveCodec::base64(data));
}
}
