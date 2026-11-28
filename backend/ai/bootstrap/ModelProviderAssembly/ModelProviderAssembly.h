#pragma once
#include "backend/ai/registry/ModelRegistryService/ModelRegistryService.h"
#include "backend/ai/inference/resilience/ResilientModelProvider/ResilientModelProvider.h"
namespace webserver::phase11 {
/** Register installed providers only. Caller owns router, validators and lifetimes. */
void configureModelProviders(ModelRouter &router, ModelRegistryService &registry, std::vector<std::shared_ptr<IModelProvider>> &providers, std::vector<std::shared_ptr<ResilientModelProvider>> &resilient, bool productionMode);
}
