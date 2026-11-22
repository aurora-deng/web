#pragma once

#include "../DagExecutor/DagExecutor.h"

namespace nebula::workflow {
// Maps trusted, validated workflow metadata to the existing Observer v1 envelope.
// Task inputs and outputs are deliberately absent from this contract.
std::string observerJson(const Event& event);
}
