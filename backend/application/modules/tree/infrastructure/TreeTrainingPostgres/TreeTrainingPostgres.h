#pragma once
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
#include "backend/application/modules/tree/ports/ITreeTrainingRepository/ITreeTrainingRepository.h"

namespace webserver::function3 {
class TreeTrainingPostgres final : public ITreeTrainingRepository {
public:
    explicit TreeTrainingPostgres(SqlContext& sql) : sql_(sql) {}
    std::optional<TreeTrainingSource> source(Id post) const override;
    bool setConsent(Id post, bool allowed) override;
    bool review(Id post, Id reviewer, const std::string& text) override;
    std::vector<TreeTrainingText> page(Id before, bool approved) const override;
private:
    SqlContext& sql_;
};
}
