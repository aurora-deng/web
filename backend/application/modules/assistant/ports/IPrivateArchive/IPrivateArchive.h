#pragma once
#include "backend/application/modules/assistant/domain/PrivateArchive/PrivateArchive.h"
namespace webserver::function3 {
class IPrivateArchive{
public:virtual~IPrivateArchive()=default;
    virtual std::vector<LegacyPrivateResource>resources(Id owner)=0;
    virtual LegacyPrivatePage exportPage(Id owner,std::string kind,Id resource,Id after,Id upper)=0;
    // 驱动持锁重新读出原页、核对摘要，再原子清除本页正文和写入幂等收据。
    virtual void purgePage(Id owner,std::string kind,Id resource,Id after,Id through,std::string checksum,
        const std::function<std::string(const LegacyPrivatePage&)>&digest)=0;
};
}
