#include "AdminPostgresRepository.h"
namespace webserver::function3 {
void AdminPostgresRepository::lockRoles(){sql_.requireTransaction();sql_.query("SELECT pg_advisory_xact_lock(3700300)");}
std::vector<SiteUser> AdminPostgresRepository::users(Id after,std::size_t limit)const{
    std::vector<SiteUser> result;
    for(auto&r:sql_.query("SELECT u.id,u.display_name,u.disabled,u.username,COALESCE(r.role,'user'),u.ai_account FROM users u LEFT JOIN site_roles r ON r.user_id=u.id WHERE u.id>$1 ORDER BY u.id LIMIT $2",{std::to_string(after),std::to_string(limit)}))result.push_back({{std::stoull(r[0]),r[1],r[4]=="administrator"?Role::Administrator:r[4]=="moderator"?Role::Moderator:Role::User,r[2]!="t",r[5]!="t"},r[3]});
    return result;
}
std::size_t AdminPostgresRepository::activeAdministrators()const{return std::stoull(sql_.query("SELECT count(*) FROM site_roles r JOIN users u ON u.id=r.user_id WHERE r.role='administrator' AND NOT u.disabled AND NOT u.ai_account")[0][0]);}
void AdminPostgresRepository::setRole(Id user,Role role){sql_.requireTransaction();sql_.query("INSERT INTO site_roles VALUES($1,$2) ON CONFLICT(user_id) DO UPDATE SET role=EXCLUDED.role",{std::to_string(user),role==Role::Administrator?"administrator":role==Role::Moderator?"moderator":"user"});}
void AdminPostgresRepository::setActive(Id user,bool active){sql_.requireTransaction();sql_.query("UPDATE users SET disabled=$2 WHERE id=$1",{std::to_string(user),active?"false":"true"});}
void AdminPostgresRepository::revokeSessions(Id user){sql_.requireTransaction();sql_.query("UPDATE user_sessions SET revoked_at=CURRENT_TIMESTAMP WHERE user_id=$1 AND revoked_at IS NULL",{std::to_string(user)});}
void AdminPostgresRepository::audit(Id actor,std::string action,Id user,std::string reason){sql_.requireTransaction();sql_.query("INSERT INTO site_audit(actor_id,action,resource,reason) VALUES($1,$2,$3,$4)",{std::to_string(actor),std::move(action),"user:"+std::to_string(user),std::move(reason)});}
std::vector<AuditRecord> AdminPostgresRepository::audits(Id before,std::size_t limit)const{
    std::vector<AuditRecord> result;for(auto&r:sql_.query("SELECT id,actor_id,action,resource,reason,created_at::text FROM site_audit WHERE ($1::bigint=0 OR id<$1) ORDER BY id DESC LIMIT $2",{std::to_string(before),std::to_string(limit)}))result.push_back({std::stoull(r[0]),std::stoull(r[1]),r[2],r[3],r[4],r[5]});return result;
}
}
