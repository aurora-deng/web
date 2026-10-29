#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <optional>
namespace webserver::function3 {
struct SpeechJob{Id announcement{};std::string text,voice;Milliseconds expiresAt{};double speed{};};
struct SpeechAudio{std::string state,cacheKey;};
class ISpeechTasks {
public:
    virtual ~ISpeechTasks()=default;
    virtual void enqueue(Id announcement,std::string text,Milliseconds expiresAt)=0;
    virtual std::optional<SpeechJob> claim()=0;
    virtual void complete(const SpeechJob&job,std::string key)=0;
    virtual void failed(const SpeechJob&job)=0;
    virtual SpeechAudio audio(Id announcement,std::string voice)const=0;
};
class ISpeechRenderer {
public:virtual ~ISpeechRenderer()=default;virtual std::string render(const SpeechJob&job)=0;virtual std::string path(std::string key)const=0;
};
}
