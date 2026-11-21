#include "TreeTrainingPostgres.h"

namespace webserver::function3 {
std::optional<TreeTrainingSource> TreeTrainingPostgres::source(Id post) const {
    const auto result = sql_.query(
        "SELECT p.author_id,(p.state='published' AND p.tree_channel AND btrim(p.body)<>''),COALESCE(t.allowed,false) "
        "FROM tree_posts p LEFT JOIN tree_training_choices t ON t.post_id=p.id WHERE p.id=$1",
        {std::to_string(post)});
    if (result.empty()) return std::nullopt;
    const auto& row = result.front();
    return TreeTrainingSource{std::stoull(row[0]), row[1] == "t", row[2] == "t"};
}

bool TreeTrainingPostgres::setConsent(Id post, bool allowed) {
    sql_.requireTransaction();
    if (!allowed) {
        return !sql_.query("DELETE FROM tree_training_choices WHERE post_id=$1 RETURNING post_id",
                           {std::to_string(post)}).empty();
    }
    return !sql_.query(
        "INSERT INTO tree_training_choices (post_id,allowed) "
        "SELECT id,true FROM tree_posts WHERE id=$1 AND state='published' AND tree_channel AND btrim(body)<>'' "
        "ON CONFLICT (post_id) DO NOTHING RETURNING post_id", {std::to_string(post)}).empty();
}

bool TreeTrainingPostgres::review(Id post, Id reviewer, const std::string& text) {
    sql_.requireTransaction();
    return !sql_.query(
        "UPDATE tree_training_choices t SET reviewed_text=$3, reviewer_id=$2 FROM tree_posts p "
        "WHERE p.id=t.post_id AND t.post_id=$1 AND t.allowed AND p.state='published' AND p.tree_channel "
        "RETURNING t.post_id", {std::to_string(post), std::to_string(reviewer), text}).empty();
}

std::vector<TreeTrainingText> TreeTrainingPostgres::page(Id before, bool approved) const {
    const auto rows = sql_.query(
        "SELECT p.id,CASE WHEN $2::boolean THEN t.reviewed_text ELSE p.body END,t.reviewed_text IS NOT NULL "
        "FROM tree_training_choices t JOIN tree_posts p ON p.id=t.post_id "
        "WHERE t.allowed AND p.state='published' AND p.tree_channel AND ($1::bigint=0 OR p.id<$1) "
        "AND (NOT $2::boolean OR t.reviewed_text IS NOT NULL) ORDER BY p.id DESC LIMIT 50",
        {std::to_string(before), approved ? "true" : "false"});
    std::vector<TreeTrainingText> result;
    result.reserve(rows.size());
    for (const auto& row : rows)
        result.push_back({std::stoull(row[0]), row[1], row[2] == "t"});
    return result;
}
}
