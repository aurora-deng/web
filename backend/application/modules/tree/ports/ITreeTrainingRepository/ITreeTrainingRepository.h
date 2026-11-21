#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <optional>
#include <vector>

namespace webserver::function3 {
struct TreeTrainingSource {
    Id author{};
    bool eligible{};
    bool allowed{};
};
struct TreeTrainingText {
    Id post{};
    std::string text;
    bool reviewed{};
};

class ITreeTrainingRepository {
public:
    virtual ~ITreeTrainingRepository() = default;
    virtual std::optional<TreeTrainingSource> source(Id post) const = 0;
    virtual bool setConsent(Id post, bool allowed) = 0;
    virtual bool review(Id post, Id reviewer, const std::string& text) = 0;
    virtual std::vector<TreeTrainingText> page(Id before, bool approved) const = 0;
};
}
