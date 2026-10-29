#pragma once
#include "backend/ai/speech/ports/ISpeechTasks/ISpeechTasks.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
class SpeechPostgresTasks final:public ISpeechTasks {
public:
    explicit SpeechPostgresTasks(SqlContext&sql,IEventWriter*events=nullptr):sql_(sql),events_(events){}
    void enqueue(Id announcement,std::string text,Milliseconds expiresAt)override;
    std::optional<SpeechJob> claim()override;
    void complete(const SpeechJob&job,std::string key)override;
    void failed(const SpeechJob&job)override;
    SpeechAudio audio(Id announcement,std::string voice)const override;
private:SqlContext&sql_;IEventWriter*events_;
};
}
