#pragma once
#include "backend/ai/speech/ports/ISpeechTasks/ISpeechTasks.h"
#include <filesystem>
#include <atomic>
#include <mutex>
#include <vector>
namespace webserver::function3 {
struct SpeechProcessConfig{std::filesystem::path executable,model,engine,cache;std::string modelDigest;};
class SpeechProcessRenderer final:public ISpeechRenderer {
public:
    explicit SpeechProcessRenderer(SpeechProcessConfig config):config_(std::move(config)){}
    std::string render(const SpeechJob&job)override;
    std::string path(std::string key)const override;
    void cancel()noexcept{cancelled_.store(true,std::memory_order_relaxed);}
    void verifyModel()const;
    void storeWave(const std::string&key,const std::vector<unsigned char>&wave);
private:
    void quota();
    SpeechProcessConfig config_;
    std::atomic<bool> cancelled_{};
    std::mutex rendering_;
};
}
