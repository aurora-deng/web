#include "RankingPostgresRepository.h"
#include <iomanip>
#include <locale>
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

RankSnapshot RankingPostgresRepository::latestCategory(Id category) const {
    const auto rows = sql_.query(
        "SELECT v.id,floor(extract(epoch FROM v.settled_at)*1000)::bigint,r.post_id,r.score "
        "FROM (SELECT id,settled_at FROM tree_rank_versions ORDER BY id DESC LIMIT 1) v LEFT JOIN LATERAL ("
        "SELECT e.post_id,e.score,e.rank FROM tree_category_rank_entries e "
        "JOIN tree_categories c ON c.id=e.category_id AND c.active "
        "JOIN tree_posts p ON p.id=e.post_id AND p.state='published' AND p.tree_channel "
        "WHERE e.version_id=v.id AND e.category_id=$1) r ON true ORDER BY r.rank", {std::to_string(category)});
    if (rows.empty()) return {};
    RankSnapshot snapshot{std::stoull(rows.front()[0]), std::stoll(rows.front()[1]), {}};
    for (const auto& row : rows)
        if (!row[2].empty()) snapshot.entries.push_back({std::stoull(row[2]), std::stod(row[3])});
    return snapshot;
}

void RankingPostgresRepository::saveCategories(Id version, const std::vector<CategoryRankEntry>& entries) {
    sql_.requireTransaction();
    sql_.query("DELETE FROM tree_category_rank_entries WHERE version_id<$1 RETURNING post_id",
               {std::to_string(version)});
    if (entries.empty()) return;
    std::ostringstream records;
    records.imbue(std::locale::classic());
    records << '[' << std::setprecision(17);
    for (const auto& item : entries) {
        if (&item != &entries.front()) records << ',';
        records << "{\"category\":" << item.category << ",\"position\":" << item.position
                << ",\"post\":" << item.entry.post << ",\"score\":" << item.entry.score << '}';
    }
    records << ']';
    sql_.query("INSERT INTO tree_category_rank_entries(version_id,category_id,rank,post_id,score) "
               "SELECT $1,category,position,post,score FROM jsonb_to_recordset($2::jsonb) "
               "AS item(category bigint,position smallint,post bigint,score double precision) RETURNING post_id",
               {std::to_string(version), records.str()});
}
}
