#pragma once

#include "../../../application/EdgeWorker/EdgeWorker.h"
#include "../FleetWire/FleetWire.h"

namespace nebula::fleet::wire {

// The context, stream and frame buffers live on this caller thread. The
// separate EdgeWorker owns execution and communicates through the ledger.
void runEdgeSessions(std::string_view address, std::string_view robotId, std::string_view token,
                     Capabilities capabilities, EdgeLedger& ledger, EdgeWorker& worker, std::stop_token stop);
}
