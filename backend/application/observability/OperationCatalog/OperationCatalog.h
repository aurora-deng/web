#pragma once
#include <array>
#include <string_view>

namespace webserver::telemetry {
struct OperationDefinition { std::string_view key, domain, title, name; };
inline constexpr auto operationDefinitions = std::to_array<OperationDefinition>({
#define NEBULA_OPERATION(enumKey, key, domain, title, name) {#key, domain, title, name},
#include "OperationCatalog.def"
#undef NEBULA_OPERATION
});
}
