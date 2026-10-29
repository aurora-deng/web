#pragma once
#include "backend/application/modules/tree/ports/ITreeMediaProcessor/ITreeMediaProcessor.h"
#include <filesystem>
namespace webserver::function3 {
class FfmpegMediaProcessor final:public ITreeMediaProcessor {
public:
    FfmpegMediaProcessor(std::filesystem::path prefix,std::filesystem::path output):prefix_(std::move(prefix)),output_(std::move(output)){}
    ValidatedMedia validate(Id file,std::string input,std::uint64_t bytes)override;
private:std::filesystem::path prefix_,output_;
};
}
