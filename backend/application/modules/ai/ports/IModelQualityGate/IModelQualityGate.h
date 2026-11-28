#pragma once
#include "backend/application/modules/shared/domain/IdentityTypes/IdentityTypes.h"
#include "backend/application/modules/ai/domain/EvaluationTypes/EvaluationTypes.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
namespace webserver::phase11 {
class IModelQualityGate
{
public:
    virtual ~IModelQualityGate() = default;
    [[nodiscard]] virtual ModelQualityGateDecision assess(
        ModelNodeId node, ModelVersionId version, TimePoint now) const = 0;
};
}
