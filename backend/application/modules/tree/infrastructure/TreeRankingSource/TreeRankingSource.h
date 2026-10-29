#pragma once
#include "backend/application/modules/ranking/ports/IRankingRepository/IRankingRepository.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
class TreeRankingSource final:public ITreeRankingSource {
public:
    explicit TreeRankingSource(SqlContext &sql):sql_(sql){}
    std::vector<RankCandidate> topCandidates(Milliseconds now) const override;
private: SqlContext &sql_;
};
}
