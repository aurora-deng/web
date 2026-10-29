#include "TreeHttp.h"
#include "backend/application/modules/activities/transport/ActivityJson/ActivityJson.h"
#include <sstream>
namespace webserver::function3 {
namespace {
std::string replyJson(const TreeReply&r){return "{\"id\":"+http::id(r.id)+",\"authorId\":"+http::id(r.author)+",\"authorName\":"+http::quote(r.authorName)+",\"body\":"+http::quote(r.body)+",\"createdAt\":"+std::to_string(r.createdAt)+",\"parentReplyId\":"+http::id(r.parent)+",\"replyToName\":"+http::quote(r.replyToName)+"}";}
std::vector<std::string> split(std::string value,char separator){std::vector<std::string>result;std::istringstream in(value);for(std::string part;std::getline(in,part,separator);)if(!part.empty())result.push_back(part);return result;}
}
void TreeHttp::registerRoutes(Router &router){
    auto scope=http::executor(router,executor_);
    router.GET("/api/admin/tree/categories",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx).user.id;http::reply(ctx,http::array(tree_.adminCategories(user),[](auto&c){return "{\"id\":"+http::id(c.id)+",\"name\":"+http::quote(c.name)+",\"active\":"+(c.active?"true":"false")+"}";}));});});
    router.POST("/api/admin/tree/categories",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx,true).user.id;auto f=http::body(ctx);auto c=tree_.category(user,http::number(http::field(f,"id","0"),true),http::field(f,"name"),http::boolean(http::field(f,"active","true")),http::now());if(cache_)cache_->invalidate("categories");http::reply(ctx,"{\"id\":"+http::id(c.id)+"}");});});
    router.GET("/api/tree/categories",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx).user.id;auto cached=cache_?cache_->lookup("categories"):CacheLookup{};if(cached.value){http::reply(ctx,*cached.value);return;}auto json=http::array(tree_.categories(user),[](const auto&c){return "{\"id\":"+http::id(c.id)+",\"name\":"+http::quote(c.name)+"}";});if(cache_)cache_->store("categories",cached.generation,json,30);http::reply(ctx,std::move(json));});});
    router.GET("/api/tree/posts",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx).user.id;PostQuery q;
        q.before=http::number(http::query(ctx,"before","0"),true);q.limit=http::number(http::query(ctx,"limit","24"));
        const auto ids=http::query(ctx,"ids");if(!ids.empty()){
            // 前十合并成一个有界请求，隐藏、删除与非树频道仍被正常过滤。
            for(const auto& id:split(ids,','))q.ids.push_back(http::number(id));
            if(q.ids.empty()||q.ids.size()>10)throw BusinessError(Failure::Invalid,"post batch must contain 1..10 ids");
        }
        auto category=http::query(ctx,"category");if(!category.empty())q.category=http::number(category);
        auto author=http::query(ctx,"author");if(!author.empty())q.author=http::number(author);
        auto since=http::query(ctx,"since");if(!since.empty())q.since=http::number(since,true);
        auto until=http::query(ctx,"until");if(!until.empty())q.until=http::number(until);
        if(q.since&&q.until&&*q.since>=*q.until)throw BusinessError(Failure::Invalid,"time range must be increasing");
        q.tag=http::query(ctx,"tag");q.mood=http::query(ctx,"mood");
        auto kind=http::query(ctx,"media");if(!kind.empty()){if(kind!="image"&&kind!="audio")throw BusinessError(Failure::Invalid,"unsupported media filter");q.mediaKind=kind=="image"?MediaKind::Image:MediaKind::Audio;}
        auto posts=tree_.list(user,q);
        http::reply(ctx,"{\"total\":"+http::id(tree_.count(user))+",\"posts\":"+http::array(posts,activityJson)+",\"next\":"+(posts.empty()?"null":http::id(posts.back().id))+"}");});});
    router.POST("/api/tree/posts",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx,true).user.id;auto f=http::body(ctx);PublishPost p;
        p.requestId=http::field(f,"requestId");p.body=http::field(f,"body");p.category=http::number(http::field(f,"categoryId"));p.mood=http::field(f,"mood");p.tags=split(http::field(f,"tags"),'\n');
        for(auto&id:split(http::field(f,"fileIds"),','))p.files.push_back(http::number(id));
        http::reply(ctx,activityJson(tree_.publish(user,std::move(p),http::now())),201);});});
    router.GET("/api/tree/posts/:post",[this](RequestContext&ctx){return http::guarded(ctx,[&]{http::reply(ctx,activityJson(tree_.get(auth_.authorizeRequest(ctx).user.id,http::path(ctx,"post"))));});});
    router.DEL("/api/tree/posts/:post",[this](RequestContext&ctx){return http::guarded(ctx,[&]{tree_.remove(auth_.authorizeRequest(ctx,true).user.id,http::path(ctx,"post"),http::now(),ctx.request.bodyData.empty()?std::string{}:http::field(http::body(ctx),"reason"));http::reply(ctx,"{\"ok\":true}");});});
    router.POST("/api/tree/posts/:post/like",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx,true).user.id;auto f=http::body(ctx);http::reply(ctx,activityJson(tree_.like(user,http::path(ctx,"post"),http::boolean(http::field(f,"liked")),http::now())));});});
    router.GET("/api/tree/posts/:post/replies",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx).user.id;http::reply(ctx,http::array(tree_.replies(user,http::path(ctx,"post"),http::number(http::query(ctx,"before","0"),true),30),replyJson));});});
    router.POST("/api/tree/posts/:post/replies",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx,true).user.id;auto f=http::body(ctx);http::reply(ctx,replyJson(tree_.reply(user,http::path(ctx,"post"),http::field(f,"requestId"),http::field(f,"body"),http::now(),http::number(http::field(f,"parentReplyId","0"),true))),201);});});
    router.DEL("/api/tree/replies/:reply",[this](RequestContext&ctx){return http::guarded(ctx,[&]{tree_.removeReply(auth_.authorizeRequest(ctx,true).user.id,http::path(ctx,"reply"),http::now());http::reply(ctx,"{\"ok\":true}");});});
    router.POST("/api/tree/posts/:post/reports",[this](RequestContext&ctx){return http::guarded(ctx,[&]{auto user=auth_.authorizeRequest(ctx,true).user.id;auto value=moderation_.report(user,http::path(ctx,"post"),http::field(http::body(ctx),"reason"));http::reply(ctx,"{\"id\":"+http::id(value.id)+"}",201);});});
}
}
