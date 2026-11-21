#include "backend/ai/speech/application/SpeechService/SpeechService.h"
#include <stdexcept>
using namespace webserver::function3;
struct Tasks final:ISpeechTasks {
    bool claimed=false;int completed=0;
    void enqueue(Id,std::string,Milliseconds)override{}
    std::optional<SpeechJob>claim()override{if(claimed)return {};claimed=true;return SpeechJob{1,"fixture","zf_001",100,1};}
    void complete(const SpeechJob&,std::string key)override{if(key=="fixture-key")++completed;}
    void failed(const SpeechJob&)override{throw std::runtime_error("unexpected renderer failure");}
    SpeechAudio audio(Id,std::string)const override{return {"ready","fixture-key"};}
};
struct Renderer final:ISpeechRenderer {
    std::string render(const SpeechJob&)override{return "fixture-key";}
    std::string path(std::string)const override{return "fixture-path";}
};
int main(){
    Tasks tasks;Renderer renderer;SpeechService service(tasks,renderer);
    if(!service.runOne()||service.runOne()||tasks.completed!=1||service.preview("zf_001")!="fixture-key")
        throw std::runtime_error("standalone speech task contract failed");
}
