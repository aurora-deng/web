#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nebula::fleet {

enum class Station : int { alpha = 1, beta = 2 };
using Capabilities = unsigned;

enum class Phase : int { queued = 1, leased, accepted, executing, succeeded, failed, uncertain };
enum class ErrorCode { invalid, missing, conflict, exhausted, offline, storage };

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const char* message);
    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

struct Command {
    std::string taskId;
    std::string traceId;
    Station station;
    std::int64_t epoch{};
    bool operator==(const Command&) const = default;
};

struct Receipt {
    std::string taskId;
    std::int64_t epoch;
    Phase phase;
    std::string outcome;
};

struct Task {
    Command command;
    std::string robotId;
    Phase phase;
    std::string outcome;
    bool connected{};
    bool needsReconciliation{};
};

bool validId(std::string_view value);
bool validTrace(std::string_view value);
bool validStation(Station station);
bool validCapabilities(Capabilities allowed);
bool hasStation(Capabilities allowed, Station station);
bool active(Phase phase);
std::string_view name(Phase phase);

void checkCommand(const Command& command);
void checkReceipt(const Receipt& receipt);
}
