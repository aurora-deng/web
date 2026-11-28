#pragma once
#include "backend/application/modules/ai/domain/KnowledgeTypes/KnowledgeTypes.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace webserver::phase11 {

struct KnowledgeCitationView
{
    RagCitation citation;
    std::string documentTitle;
    std::string sourceName;
    std::uint64_t chunkOrdinal = 0;
    std::string content;
};

} // namespace webserver::phase11
