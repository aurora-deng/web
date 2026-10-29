#pragma once
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
#include "backend/ai/speech/infrastructure/SpeechPostgresTasks/SpeechPostgresTasks.h"
#include "backend/ai/speech/infrastructure/SpeechProcessRenderer/SpeechProcessRenderer.h"
#include "backend/ai/speech/application/SpeechService/SpeechService.h"
#include <condition_variable>
#include <thread>
#include <memory>
namespace webserver::function3 {
class SpeechRuntime final {
public:
    SpeechRuntime(SqlContext&sql,IEventWriter&events);
    ~SpeechRuntime();
    void stop();
    SpeechService&service(){return service_;}
    void enqueue(Id announcement,std::string text,Milliseconds expires){tasks_.enqueue(announcement,std::move(text),expires);}
private:
    SpeechPostgresTasks tasks_;SpeechProcessRenderer renderer_;std::unique_ptr<ISpeechRenderer> cloudRenderer_;SpeechService service_;
    std::mutex mutex_;std::condition_variable wake_;bool stopping_{};std::thread worker_;
};
}
