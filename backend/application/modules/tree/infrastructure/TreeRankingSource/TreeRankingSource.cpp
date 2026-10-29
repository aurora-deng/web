#include "TreeRankingSource.h"
namespace webserver::function3 {
std::vector<RankCandidate> TreeRankingSource::topCandidates(Milliseconds now) const {
    // 单条 MVCC 查询确保点赞、回复、可见状态来自同一个快照；最多传回十条。
    auto rows=sql_.query("WITH eligible AS (SELECT p.id,p.author_id,p.created_ms FROM tree_posts p WHERE state='published' AND tree_channel AND created_ms<=$1 AND created_ms>$1::bigint-604800000),"
        "l AS (SELECT l.post_id,count(*) AS n FROM tree_likes l JOIN eligible p ON p.id=l.post_id WHERE l.user_id<>p.author_id GROUP BY l.post_id),"
        "r AS (SELECT r.post_id,count(DISTINCT r.author_id) AS n FROM tree_replies r JOIN eligible p ON p.id=r.post_id WHERE NOT r.deleted AND r.author_id<>p.author_id GROUP BY r.post_id) "
        "SELECT p.id,u.display_name,p.created_ms,COALESCE(l.n,0),COALESCE(r.n,0) FROM eligible p JOIN users u ON u.id=p.author_id LEFT JOIN l ON l.post_id=p.id LEFT JOIN r ON r.post_id=p.id "
        "ORDER BY (3.0*COALESCE(l.n,0)+2.0*COALESCE(r.n,0))/power(($1::double precision-p.created_ms)/3600000.0+2.0,1.2) DESC,p.created_ms,p.id LIMIT 10",{std::to_string(now)});
    std::vector<RankCandidate> result;
    for(auto&r:rows) result.push_back({std::stoull(r[0]),r[1],std::stoll(r[2]),std::stoull(r[3]),std::stoull(r[4])});
    return result;
}
}
