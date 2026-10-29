#pragma once
#include "backend/ai/speech/ports/ISpeechTasks/ISpeechTasks.h"
#include "backend/ai/speech/ports/ISpeechControl/ISpeechControl.h"
namespace webserver::function3 {
class SpeechService final {
public:
    SpeechService(ISpeechTasks&tasks,ISpeechRenderer&renderer):tasks_(tasks),renderer_(&renderer),localRenderer_(&renderer){}
    void setDriver(ISpeechRenderer&renderer,ISpeechControl*control){renderer_=&renderer;control_=control;}
    ISpeechControl*control()const{return control_;}
    bool runOne();
    SpeechAudio audio(Id announcement,std::string voice)const;
    std::string preview(std::string voice,bool localOnly=false,double speed=0);
    std::string audioPath(std::string key)const{return renderer_->path(std::move(key));}
private:ISpeechTasks&tasks_;ISpeechRenderer*renderer_;ISpeechRenderer*localRenderer_;ISpeechControl*control_{};
};
}
