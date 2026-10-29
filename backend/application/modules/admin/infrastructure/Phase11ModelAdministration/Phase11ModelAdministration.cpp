#include "Phase11ModelAdministration.h"
namespace webserver::function3 {
std::vector<ManagedModel>Phase11ModelAdministration::nodes(Id after)const{std::vector<ManagedModel>result;for(auto&m:models_.modelNodes(after,50))result.push_back({m.id,m.activeVersionId.value_or(0),m.name});return result;}
std::vector<ManagedVersion>Phase11ModelAdministration::versions(Id node,Id after)const{
    if(!models_.findModelNode(node))throw BusinessError(Failure::NotFound,"model node not found");
    std::vector<ManagedVersion>result;for(auto&v:models_.modelVersions(node,after,50)){using S=phase11::ModelVersionState;result.push_back({v.id,v.runtime,v.state==S::Candidate?"candidate":v.state==S::Active?"active":v.state==S::Retired?"retired":"rejected",v.checksum});}return result;
}
QualityDecision Phase11ModelAdministration::assess(Id node,Id version)const{
    auto v=models_.findModelVersion(version);if(!v||v->nodeId!=node)throw BusinessError(Failure::NotFound,"version not in this model");
    const auto d=evaluation_.assess(node,version,std::chrono::system_clock::now());return{d.allowed,d.runId.value_or(0),d.detail};
}
void Phase11ModelAdministration::change(Id node,Id version,bool rollback,std::string actor,std::function<void()>beforeCommit){if(rollback)registry_.rollback(node,version,std::move(actor),std::move(beforeCommit));else registry_.activate(node,version,std::move(actor),std::move(beforeCommit));}
}
