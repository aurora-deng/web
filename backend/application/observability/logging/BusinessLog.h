#pragma once

#include <cstdint>
#include <source_location>
#include <string_view>

namespace webserver::business_log
{
enum class Level { Trace, Debug, Info, Warn, Error, Fatal };
enum class Code { OperationCompleted, OperationFailed, HttpRejected, HttpFailed, WsRejected, WsFailed, RuntimeStarted };

// Only normalized operations and stable error codes belong here. Never pass body/SQL/what().
struct Record
{
    Code code;
    Level level = Level::Info;
    std::string_view operation{};
    int status = 0;
    std::int64_t durationUs = -1;
    std::string_view traceId{}, spanId{}, errorCode{};
};

bool enabled(Level level) noexcept;
void write(const Record &record, std::source_location location = std::source_location::current()) noexcept;
// Export worker only. Optional owned daily files, seven UTC dates, 8 MiB/day.
bool output(std::string_view record) noexcept;
}
