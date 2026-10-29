#include "AssistantModelsPostgres.h"
namespace webserver::function3 {
std::vector<AssistantModelEntry>AssistantModelsPostgres::configured()const{std::vector<AssistantModelEntry>result;for(const auto&r:sql_.query("SELECT u.id,n.id,v.id,u.display_name,v.runtime FROM users u JOIN model_bindings b ON b.ai_user_id=u.id JOIN model_nodes n ON n.id=b.model_node_id JOIN model_versions v ON v.id=n.active_version_id WHERE u.ai_account AND NOT u.disabled AND v.state='active' ORDER BY u.id LIMIT 64"))result.push_back({std::stoull(r[0]),std::stoull(r[1]),std::stoull(r[2]),r[3],r[4]});return result;}
}
