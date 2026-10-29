#pragma once
#include "backend/application/modules/tree/domain/TreePost/TreePost.h"
namespace webserver::function3 {
/** 统一正文接口，旧果实 ID 即动态 ID，频道不复制正文或互动。 */
class IActivityContent {
public:
    virtual ~IActivityContent() = default;
    virtual TreePost publish(Id actor, PublishPost content, Milliseconds now) = 0;
    virtual TreePost get(Id actor, Id content) const = 0;
    virtual std::vector<TreePost> list(Id actor, PostQuery query) const = 0;
};
}
