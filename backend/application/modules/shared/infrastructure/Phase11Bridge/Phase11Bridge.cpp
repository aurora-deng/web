#include "Phase11Bridge.h"
namespace webserver::function3 {
Actor Phase11Bridge::requireActor(Id id) const {
    auto rows=sql_.query("SELECT u.display_name,u.disabled,COALESCE(r.role,'user'),u.ai_account FROM users u LEFT JOIN site_roles r ON r.user_id=u.id WHERE u.id=$1",{std::to_string(id)});
    if(rows.empty()) throw BusinessError(Failure::Unauthenticated,"account not found");
    auto &r=rows[0];return {id,r[0],r[2]=="administrator"?Role::Administrator:r[2]=="moderator"?Role::Moderator:Role::User,r[1]!="t",r[3]!="t"};
}
TreeMedia Phase11Bridge::requireOwnedReadyMedia(Id owner,Id file) const {
    auto rows=sql_.query("SELECT v.kind,v.bytes,v.duration_ms FROM file_objects f JOIN tree_media_validation v ON v.file_id=f.id WHERE f.id=$1 AND f.owner_id=$2 AND f.state='ready'",{std::to_string(file),std::to_string(owner)});
    if(rows.empty()) throw BusinessError(Failure::Forbidden,"owned and validated media required");
    const auto &r=rows[0];return {file,r[0]=="image"?MediaKind::Image:MediaKind::Audio,std::stoull(r[1]),std::stoll(r[2])};
}
}
