#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
namespace webserver::function3 {
/** 只查询受众事实；业务层不包含好友表、HTTP 或数据库类型。 */
class IContentAudience {
public:
    virtual ~IContentAudience() = default;
    virtual bool friendsCanView(Id author, Id viewer) const = 0;
};
}
