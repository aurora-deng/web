#include "ReportPostgresRepository.h"
namespace webserver::function3 {
namespace {ContentReport parse(const std::vector<std::string>&r){return{std::stoull(r[0]),std::stoull(r[1]),std::stoull(r[2]),r[3],r[4]};}}
ContentReport ReportPostgresRepository::report(Id post,Id actor,std::string reason){sql_.requireTransaction();return parse(sql_.query("INSERT INTO content_reports(post_id,reporter_id,reason) VALUES($1,$2,$3) ON CONFLICT(post_id,reporter_id) DO UPDATE SET reporter_id=EXCLUDED.reporter_id RETURNING id,post_id,reporter_id,reason,state",{std::to_string(post),std::to_string(actor),std::move(reason)})[0]);}
std::vector<ContentReport>ReportPostgresRepository::pending(Id before,std::size_t limit)const{std::vector<ContentReport>result;for(auto&r:sql_.query("SELECT id,post_id,reporter_id,reason,state FROM content_reports WHERE state='pending' AND ($1::bigint=0 OR id<$1) ORDER BY id DESC LIMIT $2",{std::to_string(before),std::to_string(limit)}))result.push_back(parse(r));return result;}
void ReportPostgresRepository::resolve(Id report,bool dismiss){sql_.requireTransaction();if(sql_.query("UPDATE content_reports SET state=$2 WHERE id=$1 AND state='pending' RETURNING id",{std::to_string(report),dismiss?"dismissed":"resolved"}).empty())throw BusinessError(Failure::Conflict,"report already decided or missing");}
}
