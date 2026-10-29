#include "SpeechService.h"
namespace webserver::function3 {
bool SpeechService::runOne(){auto job=tasks_.claim();if(!job)return false;try{auto key=renderer_->render(*job);tasks_.complete(*job,std::move(key));}catch(...){tasks_.failed(*job);}return true;}
SpeechAudio SpeechService::audio(Id announcement,std::string voice)const{if(voice!="zf_001"&&voice!="zm_009")throw BusinessError(Failure::Invalid,"unsupported voice");return tasks_.audio(announcement,std::move(voice));}
std::string SpeechService::preview(std::string voice,bool localOnly,double speed){if(voice!="zf_001"&&voice!="zm_009")throw BusinessError(Failure::Invalid,"unsupported preset voice");return (localOnly?localRenderer_:renderer_)->render({0,"欢迎来到星云，神树将为你播报新的故事。",std::move(voice),0,speed});}
}
