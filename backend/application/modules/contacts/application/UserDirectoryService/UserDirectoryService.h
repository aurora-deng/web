#pragma once
#include "backend/application/modules/contacts/ports/IUserDirectoryRepository/IUserDirectoryRepository.h"
#include "backend/application/ports/IUserRepository/IUserRepository.h"
namespace webserver::contacts {
/** Contact discovery knows account visibility, not chat sessions or SQL. */
class UserDirectoryService final {
public:
    UserDirectoryService(phase11::IUserRepository&users,IUserDirectoryRepository&directory):users_(users),directory_(directory){}
    UserPage search(phase11::UserId actor,UserDirectoryQuery query)const;
private:
    phase11::IUserRepository&users_;
    IUserDirectoryRepository&directory_;
};
}
