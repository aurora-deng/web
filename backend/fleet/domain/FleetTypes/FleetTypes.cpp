#include "FleetTypes.h"

#include <algorithm>

namespace nebula::fleet {

Error::Error(ErrorCode code, const char* message) : std::runtime_error(message), code_(code) {}

bool validId(std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
               ch == '-' || ch == '_' || ch == '.';
    });
}

bool validTrace(std::string_view value) {
    return value.size() == 32 && value.find_first_not_of('0') != std::string_view::npos &&
           std::ranges::all_of(value, [](char ch) {
               return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
}

bool validStation(Station station) { return station == Station::alpha || station == Station::beta; }
bool validCapabilities(Capabilities allowed) { return allowed && (allowed & ~3U) == 0; }
bool hasStation(Capabilities allowed, Station station) {
    return validStation(station) && (allowed & static_cast<unsigned>(station)) != 0;
}

bool active(Phase phase) {
    return phase == Phase::leased || phase == Phase::accepted || phase == Phase::executing || phase == Phase::uncertain;
}

std::string_view name(Phase phase) {
    switch (phase) {
    case Phase::queued: return "queued";
    case Phase::leased: return "leased";
    case Phase::accepted: return "accepted";
    case Phase::executing: return "executing";
    case Phase::succeeded: return "succeeded";
    case Phase::failed: return "failed";
    case Phase::uncertain: return "uncertain";
    }
    return "invalid";
}

void checkCommand(const Command& command) {
    if (!validId(command.taskId) || !validTrace(command.traceId) || !validStation(command.station) ||
        command.epoch <= 0) throw Error(ErrorCode::invalid, "invalid fleet command");
}

void checkReceipt(const Receipt& receipt) {
    const auto phase = receipt.phase;
    const bool interim = phase == Phase::accepted || phase == Phase::executing;
    const bool terminal = phase == Phase::succeeded || phase == Phase::failed || phase == Phase::uncertain;
    if (!validId(receipt.taskId) || receipt.epoch <= 0 || (!interim && !terminal) ||
        (interim && !receipt.outcome.empty()) || (terminal && !validId(receipt.outcome))) {
        throw Error(ErrorCode::invalid, "invalid fleet receipt");
    }
}
}
