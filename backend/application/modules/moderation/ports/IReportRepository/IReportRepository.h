#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <vector>
namespace webserver::function3 {
struct ContentReport{Id id{},post{},reporter{};std::string reason,state;};
class IReportRepository {
public:
    virtual ~IReportRepository()=default;
    virtual ContentReport report(Id post,Id actor,std::string reason)=0;
    virtual std::vector<ContentReport> pending(Id before,std::size_t limit)const=0;
    virtual void resolve(Id report,bool dismiss)=0;
};
}
