#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include <optional>
namespace webserver::function3 {
struct PublicProfile {
    Id id{};std::string username,name,biography,avatarUrl;
    bool self{},ai{},showBiography{true},showAvatar{true};
    bool allowTreeFriendRequests{},friendship{},blocked{},pendingRequest{};
};
class IProfileDirectory {
public:
    virtual ~IProfileDirectory() = default;
    virtual PublicProfile profile(Id viewer,Id user)const = 0;
    virtual void configure(Id user,bool biography,bool avatar,std::optional<bool> treeRequests = {}) = 0;
};
}
