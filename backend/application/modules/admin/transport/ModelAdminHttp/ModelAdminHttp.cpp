#include "ModelAdminHttp.h"
namespace webserver::function3 {
Id ModelAdminHttp::enqueue(Id actor,Id node,Id version,bool rollback,std::string reason){
    service_.requireAdministrator(actor);
    auto state=state_;std::lock_guard lock(state->mutex);const auto now=http::now();
    for(auto i=state->operations.begin();i!=state->operations.end();){
        if(i->second.state!="pending"&&i->second.updated<now-600000)i=state->operations.erase(i);else ++i;
    }
    for(auto&[id,job]:state->operations)if(job.state=="pending"&&job.actor==actor&&job.node==node&&job.version==version&&job.rollback==rollback)return id;
    if(state->operations.size()>=64)throw BusinessError(Failure::Busy,"model operation status capacity reached");
    const auto id=++state->next;state->operations.emplace(id,Operation{actor,node,version,rollback,now});auto*service=&service_;
    // 接受的是独立后台操作，HTTP 生命周期结束不代表取消发布。最终事实仍在模型审计中。
    if(!executor_.submit([state,service,id,actor,node,version,rollback,reason=std::move(reason)]{
        std::string result="completed";try{service->change(actor,node,version,rollback,reason);}catch(...){result="failed";}
        std::lock_guard guard(state->mutex);auto&job=state->operations.at(id);job.state=std::move(result);job.updated=http::now();
    })){state->operations.erase(id);throw BusinessError(Failure::Busy,"model operation queue full");}
    return id;
}
void ModelAdminHttp::registerRoutes(Router&router){
    // 元数据和状态查询不排在试装后面；只有后台操作使用单 Worker / 4 项队列。
    router.GET("/api/admin/model-operations/:operation",[this](RequestContext&c){return http::guarded(c,[&]{auto actor=auth_.authorizeRequest(c).user.id;service_.requireAdministrator(actor);auto id=http::path(c,"operation");std::lock_guard lock(state_->mutex);auto i=state_->operations.find(id);if(i==state_->operations.end())throw BusinessError(Failure::NotFound,"operation status expired or process restarted; inspect model version and audit before retrying");if(i->second.actor!=actor)throw BusinessError(Failure::Forbidden,"operation belongs to another administrator");http::reply(c,"{\"id\":"+http::id(id)+",\"state\":"+http::quote(i->second.state)+"}");});});
    router.GET("/api/admin/models",[this](RequestContext&c){return http::guarded(c,[&]{auto user=auth_.authorizeRequest(c).user.id;http::reply(c,http::array(service_.nodes(user,http::number(http::query(c,"after","0"),true)),[](auto&m){return "{\"id\":"+http::id(m.id)+",\"name\":"+http::quote(m.name)+",\"activeVersion\":"+http::id(m.activeVersion)+"}";}));});});
    router.GET("/api/admin/models/:node/versions",[this](RequestContext&c){return http::guarded(c,[&]{auto user=auth_.authorizeRequest(c).user.id;http::reply(c,http::array(service_.versions(user,http::path(c,"node"),http::number(http::query(c,"after","0"),true)),[](auto&v){return "{\"id\":"+http::id(v.id)+",\"runtime\":"+http::quote(v.runtime)+",\"state\":"+http::quote(v.state)+",\"checksum\":"+http::quote(v.checksum)+"}";}));});});
    router.GET("/api/admin/models/:node/versions/:version/quality",[this](RequestContext&c){return http::guarded(c,[&]{auto d=service_.assess(auth_.authorizeRequest(c).user.id,http::path(c,"node"),http::path(c,"version"));http::reply(c,"{\"allowed\":"+std::string(d.allowed?"true":"false")+",\"runId\":"+http::id(d.run)+",\"reason\":"+http::quote(d.reason)+"}");});});
    router.POST("/api/admin/models/:node/versions/:version",[this](RequestContext&c){return http::guarded(c,[&]{auto user=auth_.authorizeRequest(c,true).user.id;auto f=http::body(c);auto action=http::field(f,"action");if(action!="activate"&&action!="rollback")throw BusinessError(Failure::Invalid,"activate or rollback required");auto id=enqueue(user,http::path(c,"node"),http::path(c,"version"),action=="rollback",http::field(f,"reason"));http::reply(c,"{\"operationId\":"+http::id(id)+",\"state\":\"pending\"}",202);});});
}
}
