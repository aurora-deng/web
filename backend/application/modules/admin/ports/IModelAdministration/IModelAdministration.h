#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <vector>
namespace webserver::function3 {
struct ManagedModel { Id id{},activeVersion{};std::string name; };
struct ManagedVersion { Id id{};std::string runtime,state,checksum; };
struct QualityDecision { bool allowed{};Id run{};std::string reason; };
/** 站点模块只看模型公开控制端口，不读取模型表或绕过原有质量门禁。 */
class IModelAdministration {
public:
    virtual ~IModelAdministration()=default;
    virtual std::vector<ManagedModel>nodes(Id after)const=0;
    virtual std::vector<ManagedVersion>versions(Id node,Id after)const=0;
    virtual QualityDecision assess(Id node,Id version)const=0;
    virtual void change(Id node,Id version,bool rollback,std::string actor,std::function<void()>beforeCommit)=0;
};
}
