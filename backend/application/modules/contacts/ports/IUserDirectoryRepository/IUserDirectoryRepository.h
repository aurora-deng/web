#pragma once
#include "backend/application/modules/contacts/domain/ContactDirectoryTypes/ContactDirectoryTypes.h"
#include <string_view>
namespace webserver::contacts {
class IUserDirectoryRepository {
public:
    virtual ~IUserDirectoryRepository()=default;
    virtual std::vector<UserCard>searchUsers(phase11::UserId viewer,std::string_view prefix,phase11::UserId after,std::size_t limit)const=0;
};
}
