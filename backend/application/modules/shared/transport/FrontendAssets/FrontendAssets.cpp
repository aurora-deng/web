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
        const std::string mime=extension==".js"?"text/javascript; charset=utf-8":extension==".css"?"text/css; charset=utf-8":extension==".html"?"text/html; charset=utf-8":"";
        if(mime.empty())continue;
        const auto relative=entry.path().lexically_relative(root).generic_string();
        auto handler=[path=entry.path().string(),mime](RequestContext &ctx){
            ctx.response->setHeader("Content-Type",mime);ctx.response->setHeader("Cache-Control","no-cache");ctx.response->setHeader("X-Content-Type-Options","nosniff");
            if(!ctx.response->sendfile(path,ctx.request,ctx.request.range)){ctx.response->status=404;ctx.response->statusText="Not Found";ctx.response->text("Not Found");}return true;
        };
        router.GET("/phase11/"+relative,handler);
        if(relative=="index.html"){router.GET("/phase11",handler);router.GET("/phase11/",handler);}
        else if(extension==".html" && entry.path().filename()=="index.html")router.GET("/phase11/"+relative.substr(0,relative.size()-10),handler);
    }
}
}
