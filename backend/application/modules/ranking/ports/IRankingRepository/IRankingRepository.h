#pragma once
#include "backend/application/modules/ranking/domain/Ranking/Ranking.h"
namespace webserver::function3 {
class ITreeRankingSource {
public:
    virtual ~ITreeRankingSource()=default;
    /** 树模块提供有界 Top 候选视图，不向 Ranking 暴露内部表。 */
    virtual std::vector<RankCandidate> topCandidates(Milliseconds now) const=0;
};
class IRankingRepository {
public:
    virtual ~IRankingRepository()=default;
    virtual Milliseconds databaseNow() const=0;
    virtual bool lockSettlementSlot(Milliseconds slot)=0;
    virtual RankSnapshot latest() const=0;
    virtual RankSnapshot save(Milliseconds slot,Milliseconds now,const std::vector<RankEntry> &entries)=0;
};
class IRankAnnouncement {
public:
    virtual ~IRankAnnouncement()=default;
    virtual void rankingChanged(const RankSnapshot &snapshot,const std::vector<RankCandidate> &candidates)=0;
};
}
