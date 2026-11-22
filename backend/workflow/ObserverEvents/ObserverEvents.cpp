#include "ObserverEvents.h"

#include <iomanip>
#include <locale>
#include <sstream>

namespace nebula::workflow {
namespace {
std::string identity(std::string_view traceId, std::string_view runId, std::string_view nodeId, unsigned attempt) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto field : {traceId, runId, nodeId}) {
        for (const char ch : field) {
            value ^= static_cast<unsigned char>(ch);
            value *= 1099511628211ULL;
        }
        value ^= 255U;
        value *= 1099511628211ULL;
    }
    value ^= attempt;
    value *= 1099511628211ULL;
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << (value ? value : 1U);
    return out.str();
}
}

std::string observerJson(const Event& event) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    constexpr std::string_view kinds[]{"dag.run", "dag.edge", "dag.node"};
    // Only restricted identifiers and enum names are produced by DagExecutor.
    out << R"({"schemaVersion":1,"eventId":")" << event.traceId << '.'
        << identity(event.traceId, event.runId, {}, 0) << '.' << event.sequence
        << R"(","eventType":")" << kinds[static_cast<std::size_t>(event.kind)]
        << R"(","occurredAt":")" << event.occurredNs << R"(","traceId":")" << event.traceId << '"';
    if (event.kind == EventKind::node) {
        out << R"(,"spanId":")" << identity(event.traceId, event.runId, event.nodeId, event.attempt) << '"';
    }
    out << R"(,"fields":{"runId":")" << event.runId << '"';
    switch (event.kind) {
    case EventKind::run:
        out << R"(,"state":")" << stateName(event.state) << R"(","workflow":")" << event.workflow << '"';
        break;
    case EventKind::edge:
        out << R"(,"predecessor":")" << event.predecessor << R"(","successor":")" << event.nodeId << '"';
        break;
    case EventKind::node:
        out << R"(,"nodeId":")" << event.nodeId << R"(","attempt":)" << event.attempt
            << R"(,"state":")" << stateName(event.state) << '"';
        if (event.state == State::started) out << R"(,"inputCount":)" << event.inputCount;
        if (event.durationMs > 0) out << R"(,"durationMs":)" << event.durationMs;
        if (!event.errorCode.empty()) out << R"(,"errorCode":")" << event.errorCode << '"';
        break;
    }
    out << "}}";
    return out.str();
}
}
