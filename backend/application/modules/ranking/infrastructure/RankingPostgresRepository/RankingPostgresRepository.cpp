#include "RankingPostgresRepository.h"
#include <iomanip>
#include <sstream>
namespace webserver::function3 {
Milliseconds RankingPostgresRepository::databaseNow() const {return std::stoll(sql_.query("SELECT floor(extract(epoch FROM clock_timestamp())*1000)::bigint")[0][0]);}
bool RankingPostgresRepository::lockSettlementSlot(Milliseconds slot) {
    sql_.requireTransaction();
    if(sql_.query("SELECT pg_try_advisory_xact_lock(3700327)")[0][0]!="t") return false;
    return sql_.query("SELECT 1 FROM tree_rank_versions WHERE slot=$1",{std::to_string(slot)}).empty();
}
RankSnapshot RankingPostgresRepository::latest() const {
    RankSnapshot result;
    auto v=sql_.query("SELECT id,floor(extract(epoch FROM settled_at)*1000)::bigint FROM tree_rank_versions ORDER BY id DESC LIMIT 1");
    if(v.empty()) return result;
    result.version=std::stoull(v[0][0]);result.settledAt=std::stoll(v[0][1]);
    for(auto &r:sql_.query("SELECT post_id,score FROM tree_rank_entries WHERE version_id=$1 ORDER BY rank",{v[0][0]})) result.entries.push_back({std::stoull(r[0]),std::stod(r[1])});
    return result;
}
RankSnapshot RankingPostgresRepository::save(Milliseconds slot,Milliseconds now,const std::vector<RankEntry>&entries) {
    sql_.requireTransaction();
    Id id=std::stoull(sql_.query("INSERT INTO tree_rank_versions(slot) VALUES($1) RETURNING id",{std::to_string(slot)})[0][0]);
    for(std::size_t i=0;i<entries.size();++i) {
        std::ostringstream score;score<<std::setprecision(17)<<entries[i].score;
        sql_.query("INSERT INTO tree_rank_entries VALUES($1,$2,$3,$4)",{std::to_string(id),std::to_string(i+1),std::to_string(entries[i].post),score.str()});
    }
    return {id,now,entries};
}
}
