#include "FrontendAssets.h"
#include "backend/server/Route/Router.h"
#include <filesystem>
namespace webserver::function3 {
void registerFrontendAssets(Router &router) {
    const std::filesystem::path root="./frontend/phase11";
    if(!std::filesystem::exists(root))return;
    // 仅启动时枚举受版本控制的前端文件；请求中的路径从不拼接成磁盘路径。
    for(auto &entry:std::filesystem::recursive_directory_iterator(root)) {
        if(entry.is_symlink()||!entry.is_regular_file())continue;
        const auto extension=entry.path().extension().string();
        std::string mime;
        if(extension==".js") mime="text/javascript; charset=utf-8";
        else if(extension==".css") mime="text/css; charset=utf-8";
        else if(extension==".html") mime="text/html; charset=utf-8";
        else if(extension==".png") mime="image/png";
        else if(extension==".jpg" || extension==".jpeg") mime="image/jpeg";
        else if(entry.path().filename()=="asset-manifest.json") mime="application/json; charset=utf-8";
        if(mime.empty())continue;
        const auto relative=entry.path().lexically_relative(root).generic_string();
        if(relative.starts_with("agent-control/") || relative.starts_with("admin/permissions/"))continue;
        auto handler=[path=entry.path().string(),mime,extension](RequestContext &ctx){
            const bool immutable = ctx.request.querryParams.contains("v") &&
                                   (extension==".js" || extension==".css" || extension==".jpg" || extension==".jpeg" || extension==".png");
            ctx.response->setHeader("Content-Type",mime);ctx.response->setHeader("Cache-Control",immutable ? "public,max-age=31536000,immutable" : "no-cache");ctx.response->setHeader("X-Content-Type-Options","nosniff");
            if(!ctx.response->sendfile(path,ctx.request,ctx.request.range)){ctx.response->status=404;ctx.response->statusText="Not Found";ctx.response->text("Not Found");return true;}
            // Keep the frozen HTTP core unchanged: its legacy typo misses the
            // standard If-None-Match key. Complete the 304 negotiation at the
            // static-asset boundary after sendfile has supplied the ETag.
            const auto requestTag=ctx.request.headers.find("if-none-match");
            const auto responseTag=ctx.response->headers.find("ETag");
            if(requestTag!=ctx.request.headers.end() && responseTag!=ctx.response->headers.end() && requestTag->second==responseTag->second){
                ctx.response->status=304;ctx.response->statusText="Not Modified";ctx.response->body=nullptr;
            }
            return true;
        };
        router.GET("/phase11/"+relative,handler);
        if(relative=="index.html"){router.GET("/phase11",handler);router.GET("/phase11/",handler);}
        else if(extension==".html" && entry.path().filename()=="index.html")router.GET("/phase11/"+relative.substr(0,relative.size()-10),handler);
    }
}
}
