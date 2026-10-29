#include "backend/application/modules/admin/application/ModelAdminService/ModelAdminService.h"
#include "backend/application/modules/tree/infrastructure/InMemoryTreeRepository/InMemoryTreeRepository.h"
#include "backend/application/storage/InMemoryStore/InMemoryStore.h"
#include "backend/ai/models/ModelRegistryService/ModelRegistryService.h"
#include <iostream>
using namespace webserver::function3;
void check(bool condition,const char*message){if(!condition)throw std::runtime_error(message);}
struct Models:IModelAdministration{
    bool changed{};std::function<void()>duringValidation;
    std::vector<ManagedModel>nodes(Id)const override{return{{1,0,"test"}};}
    std::vector<ManagedVersion>versions(Id,Id)const override{return{{2,"test","candidate","checksum"}};}
    QualityDecision assess(Id,Id)const override{return{false,0,"no evaluation evidence"};}
    void change(Id,Id,bool,std::string,std::function<void()>beforeCommit)override{if(duringValidation)duringValidation();beforeCommit();changed=true;}
};
struct Validator:webserver::phase11::IModelArtifactValidator{
    std::string supportedRuntime()const override{return "onnx-inprocess";}
    webserver::phase11::ModelArtifactValidation validateVersion(const webserver::phase11::ModelVersion&)override{return{true,{}};}
};
int main(){try{
    InMemoryTreeRepository store;store.seedActor({1,"normal"});store.seedActor({9,"site-admin",Role::Administrator});Models models;ModelAdminService service(store,models,store,store);
    try{service.nodes(1,0);throw std::logic_error("normal user accessed models");}catch(const BusinessError&e){check(e.code==Failure::Forbidden,"wrong permission");}
    check(service.nodes(9,0).size()==1&&!service.assess(9,1,2).allowed,"gate facts");
    models.duringValidation=[&]{store.seedActor({9,"revoked",Role::User});};
    try{service.change(9,1,2,false,"试装期间被吊销权限");throw std::logic_error("revoked administrator committed");}catch(const BusinessError&e){check(e.code==Failure::Forbidden&&!models.changed,"late permission check");}
    check(!store.events().empty(),"operation intention audited");
    // 原 ModelRegistry 的提交前钩子也必须执行，不能只在外围做一次检查。
    webserver::phase11::InMemoryStore registryStore;webserver::phase11::ModelRegistryService registry(registryStore);registry.registerValidator(std::make_shared<Validator>());
    auto now=std::chrono::system_clock::now();auto node=registry.createNode("test",webserver::phase11::ModelNodeKind::Root,now);
    auto version=registry.registerVersion(node.id,"onnx-inprocess","fixture",{},std::string(64,'a'),now);
    try{registry.activate(node.id,version.id,"test",[]{throw BusinessError(Failure::Forbidden,"revoked");});throw std::logic_error("registry ignored hook");}catch(const BusinessError&){}
    check(!registryStore.findModelNode(node.id)->activeVersionId,"old active pointer preserved");
    registry.activate(node.id,version.id,"test",[]{});check(registryStore.findModelNode(node.id)->activeVersionId==version.id,"authorized commit");
    std::cout<<"Model management ACL, audit intention, real registry late authorization and active-pointer preservation passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
