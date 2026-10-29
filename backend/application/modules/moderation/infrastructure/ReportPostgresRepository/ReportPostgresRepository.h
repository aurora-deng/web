#pragma once
#include "backend/application/modules/moderation/ports/IReportRepository/IReportRepository.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"
namespace webserver::function3 {
class ReportPostgresRepository final:public IReportRepository {
public:
    explicit ReportPostgresRepository(SqlContext&sql):sql_(sql){}
    ContentReport report(Id post,Id actor,std::string reason)override;
    std::vector<ContentReport>pending(Id before,std::size_t limit)const override;
    void resolve(Id report,bool dismiss)override;
private:SqlContext&sql_;
};
}
