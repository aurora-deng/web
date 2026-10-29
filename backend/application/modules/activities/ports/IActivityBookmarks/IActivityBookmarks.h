#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <set>
#include <vector>
namespace webserver::function3 {
class IActivityBookmarks {
public:
    virtual ~IActivityBookmarks() = default;
    virtual void set(Id user,Id post,bool enabled,Milliseconds now) = 0;
    virtual std::set<Id> contained(Id user,const std::vector<Id>&posts) const = 0;
    virtual std::vector<Id> page(Id user,Id before,std::size_t limit)const = 0;
};
}
