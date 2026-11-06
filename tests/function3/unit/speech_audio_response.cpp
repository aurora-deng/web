#include "backend/ai/speech/transport/SpeechHttp/AudioResponse.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace webserver::function3;
void check(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
int main(){
    char pattern[]="/tmp/nebula-audio-XXXXXX";const auto*created=mkdtemp(pattern);if(!created)return 1;
    const std::filesystem::path root(created),file=root/"preview.wav";
    try{
        auto wave=nebula::speech::WaveCodec::pcm24k({0,0,12,0});{std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(wave.data()),wave.size());}
        HttpResponse response;RequestContext context;context.response=&response;
        http::speechAudio(context,file.string());check(response.status==200,"audio status changed");
        check(response.headers.at("Content-Type")=="audio/wav","MIME type lost");
        check(response.body&&response.body->remain()==wave.size(),"binary WAV was truncated");
        check(!response.body->useSendfile(),"H2 still receives an unsupported sendfile body");
        wave=nebula::speech::WaveCodec::pcm24k(std::vector<unsigned char>(24000*2*30));
        {std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(wave.data()),wave.size());}
        context.request.version="HTTP/2";bool needsRange=false;try{http::speechAudio(context,file.string());}catch(const BusinessError&){needsRange=true;}check(needsRange,"large H2 response bypassed range requirement");
        context.request.headers["range"]="bytes=0-524287";http::speechAudio(context,file.string());
        check(response.status==206&&response.body->remain()==512*1024,"large H2 audio exceeds bounded response cap");
        context.request.headers["range"]="bytes=524288-1048575";http::speechAudio(context,file.string());
        check(response.headers.at("Content-Range")=="bytes 524288-1048575/"+std::to_string(wave.size()),"audio cursor changed");
        context.request.headers["range"]="bytes=9999999-";http::speechAudio(context,file.string());check(response.status==416,"invalid range accepted");
        context.request.headers.clear();
        bool rejected=false;try{http::speechAudio(context,(root/"missing").string());}catch(const BusinessError&){rejected=true;}check(rejected,"missing audio accepted");
        {std::ofstream out(file);out<<"<html>authentication error pretending to be audio</html>";}
        rejected=false;try{http::speechAudio(context,file.string());}catch(const BusinessError&){rejected=true;}check(rejected,"non-WAV cache accepted");
        std::filesystem::remove_all(root);std::cout<<"PASS bounded binary audio response, no sendfile, MIME and invalid cache rejection\n";
    }catch(const std::exception&e){std::filesystem::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}
}
