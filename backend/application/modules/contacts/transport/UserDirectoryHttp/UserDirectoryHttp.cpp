#include "UserDirectoryHttp.h"
namespace webserver::contacts {
void UserDirectoryHttp::registerRoutes(Router&router){
    namespace http=webserver::function3::http;
    auto execution=http::executor(router,executor_);
    router.GET("/api/users/search",[this](RequestContext&ctx){return http::guarded(ctx,[&]{
        const auto actor=auth_.authorizeRequest(ctx).user.id;
        const UserDirectoryQuery query{http::query(ctx,"q"),http::number(http::query(ctx,"after","0"),true),http::number(http::query(ctx,"limit","20"))};
        const auto page=service_.search(actor,query);
        http::reply(ctx,"{\"items\":"+http::array(page.items,[](const UserCard&u){
            return "{\"id\":"+http::id(u.id)+",\"username\":"+http::quote(u.username)+",\"displayName\":"+http::quote(u.displayName)+",\"avatarUrl\":"+(u.avatar?http::quote("/api/files/"+std::to_string(*u.avatar)):"null")+",\"aiAccount\":"+(u.ai?"true":"false")+",\"friendship\":"+(u.friendship?"true":"false")+",\"pendingRequest\":"+(u.pending?"true":"false")+",\"canRequestFriendship\":"+(!u.friendship&&!u.pending?"true":"false")+"}";
        })+",\"nextCursor\":"+http::id(page.nextCursor)+"}");
    });});
}
}
