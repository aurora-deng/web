#pragma once
#include "backend/application/modules/ranking/ports/IRankingRepository/IRankingRepository.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
class RankingPostgresRepository final:public IRankingRepository {
public:
    explicit RankingPostgresRepository(SqlContext &sql):sql_(sql){}
    Milliseconds databaseNow() const override;
    bool lockSettlementSlot(Milliseconds slot) override;
    RankSnapshot latest() const override;
    RankSnapshot save(Milliseconds slot,Milliseconds now,const std::vector<RankEntry>&entries) override;
private: SqlContext &sql_;
};
}
