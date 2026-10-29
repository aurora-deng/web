#pragma once
#include <chrono>
#include <string>
#include <vector>
#include <atomic>
namespace webserver::function3 {
struct ProcessResult{int status{};bool timedOut{};std::string output;bool cancelled{};};
/** 只给文件/TTS 专用 Worker 使用；参数逐个传给 exec，不经过 shell。 */
ProcessResult runProcess(const std::vector<std::string>&arguments,std::chrono::seconds timeout,
    std::size_t memoryMiB=1024,const std::vector<std::string>&environment={},
    const std::atomic<bool>*cancel=nullptr);
}
