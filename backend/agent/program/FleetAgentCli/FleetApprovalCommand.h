#pragma once

#include "../../infrastructure/FleetReviewGrpc/FleetReviewGrpc.h"
#include "backend/workflow/DagExecutor/DagExecutor.h"

namespace nebula::agent::cli {

int stageReview(std::string_view address, const fleet::ReviewDraft& draft, workflow::RunOptions options);
int review(std::string_view address, std::string_view action, workflow::RunOptions options);

}
