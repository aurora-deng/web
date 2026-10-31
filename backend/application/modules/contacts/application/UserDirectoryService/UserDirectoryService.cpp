#include "UserDirectoryService.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
namespace webserver::contacts {
UserPage UserDirectoryService::search(phase11::UserId actor,UserDirectoryQuery input)const {
    using namespace phase11;
    auto user=users_.findUser(actor);
    if(!user||user->disabled)throw ApplicationError(ErrorCode::Unauthenticated,"active account required");
    auto&[query,after,limit]=input;
    const auto start=query.find_first_not_of(" \t\r\n");
    query=start==std::string::npos?"":query.substr(start,query.find_last_not_of(" \t\r\n")-start+1);
    if(query.empty()||query.size()>256)throw ApplicationError(ErrorCode::InvalidArgument,"query must contain 1..256 bytes");
    if(!limit||limit>50)throw ApplicationError(ErrorCode::InvalidArgument,"page limit outside allowed range");
    UserPage result{directory_.searchUsers(actor,query,after,limit+1),0};
    if(result.items.size()>limit){result.items.resize(limit);result.nextCursor=result.items.back().id;}
    return result;
}
}
