#pragma once

#include "../../infrastructure/FleetReviewGrpc/FleetReviewGrpc.h"
#include "backend/workflow/DagExecutor/DagExecutor.h"
#include "backend/workflow/DurableWorkflow/DurableWorkflow.h"

namespace nebula::agent::cli {

int stageReview(std::string_view address, const fleet::ReviewDraft& draft, workflow::RunOptions options,
                workflow::DurableWorkflow& durable);
int reconcileStage(std::string_view address, std::string_view traceId, workflow::DurableWorkflow& durable,
                   workflow::WorkflowJournal& journal);
int review(std::string_view address, std::string_view action, workflow::RunOptions options);

}
