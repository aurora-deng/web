#include <sherpa-onnx/c-api/c-api.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>
#include <cmath>
#include "backend/ai/speech/application/SpeechText/SpeechText.h"
int main(int argc,char**argv){
    if(argc!=5&&argc!=6){std::cerr<<"Usage: function3-tts-worker MODEL_DIR zf_001|zm_009 TEXT_FILE OUTPUT_WAV [SPEED]\n";return 2;}
    try{
        const std::filesystem::path root=argv[1];const std::string voice=argv[2];
        if(voice!="zf_001"&&voice!="zm_009")return 2;
        const double speed=argc==6?std::stod(argv[5]):(voice=="zf_001"?.95:.94);
        if(!std::isfinite(speed)||speed<.75||speed>1.25)return 2;
        std::ifstream textFile(argv[3],std::ios::binary);std::string text((std::istreambuf_iterator<char>(textFile)),{});if(text.empty()||text.size()>4096)return 2;
        auto model=(root/"model.int8.onnx").string(),voices=(root/"voices.bin").string(),tokens=(root/"tokens.txt").string(),data=(root/"espeak-ng-data").string();
        auto lexicon=(root/"lexicon-us-en.txt").string()+","+(root/"lexicon-zh.txt").string();
        SherpaOnnxOfflineTtsConfig config{};config.model.kokoro.model=model.c_str();config.model.kokoro.voices=voices.c_str();config.model.kokoro.tokens=tokens.c_str();config.model.kokoro.data_dir=data.c_str();config.model.kokoro.lexicon=lexicon.c_str();config.model.kokoro.length_scale=1;config.model.kokoro.lang="zh";config.model.num_threads=2;config.model.provider="cpu";config.max_num_sentences=1;config.silence_scale=.2f;
        std::unique_ptr<const SherpaOnnxOfflineTts,decltype(&SherpaOnnxDestroyOfflineTts)>tts(SherpaOnnxCreateOfflineTts(&config),SherpaOnnxDestroyOfflineTts);if(!tts)return 3;
        SherpaOnnxGenerationConfig generation{};generation.sid=voice=="zf_001"?3:58;generation.speed=static_cast<float>(speed);generation.silence_scale=.2f;
        // 模型只加载一次。语义短句分别合成，在句间补短停顿，避免长公告一口气读完。
        // Kokoro 没有情绪/角色控制接口；此处只改善读法、语速与停顿，不伪造情绪能力。
        std::vector<float>samples;int rate=0;
        const auto phrases=nebula::speech::SpeechText::segments(std::move(text));
        for(std::size_t i=0;i<phrases.size();++i){
            std::unique_ptr<const SherpaOnnxGeneratedAudio,decltype(&SherpaOnnxDestroyOfflineTtsGeneratedAudio)>audio(SherpaOnnxOfflineTtsGenerateWithConfig(tts.get(),phrases[i].c_str(),&generation,nullptr,nullptr),SherpaOnnxDestroyOfflineTtsGeneratedAudio);
            if(!audio||audio->n<=0||audio->sample_rate<16000||audio->sample_rate>48000||(rate&&rate!=audio->sample_rate))return 4;
            rate=audio->sample_rate;
            const auto pause=i+1<phrases.size()?rate*18/100:0;
            if(samples.size()+audio->n+pause>static_cast<std::size_t>(rate)*60)return 4;
            samples.insert(samples.end(),audio->samples,audio->samples+audio->n);
            samples.insert(samples.end(),pause,0.f);
        }
        if(samples.empty()||!SherpaOnnxWriteWave(samples.data(),static_cast<int>(samples.size()),rate,argv[4]))return 5;
        std::cout<<voice<<" generated "<<samples.size()<<" samples at "<<rate<<" Hz\n";
    }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
