#pragma once
#include "backend/application/modules/tree/ports/ITreeRepository/ITreeRepository.h"
#include "backend/application/modules/shared/infrastructure/SqlContext/SqlContext.h"

namespace webserver::function3 {
/** 防腐层：复用旧账号/媒体事实；TreeService 不读取别的模块表。 */
class Phase11Bridge final : public IActorDirectory,public ITreeMediaCatalog {
public:
    explicit Phase11Bridge(SqlContext &sql):sql_(sql){}
    Actor requireActor(Id id) const override;
    TreeMedia requireOwnedReadyMedia(Id owner,Id file) const override;
private:
    SqlContext &sql_;
};
}
