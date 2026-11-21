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

std::vector<CategoryRankEntry> TreeRankingSource::categoryTop(Milliseconds now) const {
    const auto rows = sql_.query(
        "WITH eligible AS (SELECT p.id,p.category_id,p.author_id,p.created_ms "
        "FROM tree_posts p JOIN tree_categories c ON c.id=p.category_id AND c.active "
        "WHERE p.state='published' AND p.tree_channel AND p.created_ms<=$1 AND p.created_ms>$1::bigint-604800000), "
        "likes AS (SELECT l.post_id,count(*) n FROM tree_likes l JOIN eligible e ON e.id=l.post_id "
        "WHERE l.user_id<>e.author_id GROUP BY l.post_id), "
        "replies AS (SELECT r.post_id,count(DISTINCT r.author_id) n FROM tree_replies r JOIN eligible e ON e.id=r.post_id "
        "WHERE NOT r.deleted AND r.author_id<>e.author_id GROUP BY r.post_id), "
        "scored AS (SELECT e.id,e.category_id,e.created_ms,"
        "(3.0*COALESCE(l.n,0)+2.0*COALESCE(r.n,0))/power(($1::double precision-e.created_ms)/3600000.0+2.0,1.2) score "
        "FROM eligible e LEFT JOIN likes l ON l.post_id=e.id LEFT JOIN replies r ON r.post_id=e.id), "
        "ranked AS (SELECT *,row_number() OVER (PARTITION BY category_id ORDER BY score DESC,created_ms,id) position FROM scored) "
        "SELECT category_id,position,id,score FROM ranked WHERE position<=10 ORDER BY category_id,position",
        {std::to_string(now)});
    std::vector<CategoryRankEntry> result;
    result.reserve(rows.size());
    for (const auto& row : rows)
        result.push_back({std::stoull(row[0]), static_cast<unsigned>(std::stoul(row[1])),
                          {std::stoull(row[2]), std::stod(row[3])}});
    return result;
}
}
