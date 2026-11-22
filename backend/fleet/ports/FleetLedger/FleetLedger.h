#pragma once

#include "../../domain/FleetTypes/FleetTypes.h"

#include <optional>

namespace nebula::fleet {

class FleetLedger {
public:
    virtual ~FleetLedger() = default;

    virtual Task submit(std::string_view taskId, Station station, std::string_view traceId) = 0;
    virtual std::optional<Task> lookup(std::string_view taskId) = 0;
    virtual std::optional<Command> dispatch(std::string_view robotId, Capabilities capabilities) = 0;
    virtual void apply(std::string_view robotId, const Receipt& receipt) = 0;
};

class EdgeLedger {
public:
    virtual ~EdgeLedger() = default;

    virtual void stage(const Command& command) = 0;
    virtual std::optional<Receipt> pendingReceipt() = 0;
    virtual void confirm(const Receipt& receipt) = 0;
    virtual std::optional<Command> beginReady() = 0;
    virtual void finish(const Command& command, Phase phase, std::string_view outcome) = 0;
};
}
