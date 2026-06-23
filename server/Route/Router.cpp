#include "Router.h"

// 解析path
static std::vector<std::string> splitPath(const std::string &path)
{
    std::vector<std::string> res;
    std::stringstream ss(path);
    std::string item;

    while(std::getline(ss,item,'/'))
    {
        if(!item.empty())
            res.push_back(item);
    }

    return res;
}

void Router::GET(const std::string &path, Handler handler)
{

    RouteEntry entry;
    entry.method="GET";
    entry.path=path;
    entry.parts=splitPath(path);
    entry.handler=std::move(handler);
    routes.push_back(std::move(entry));
}

void Router::POST(const std::string &path, Handler handler)
{
    RouteEntry entry;
    entry.method="POST";
    entry.path=path;
    entry.parts=splitPath(path);
    entry.handler=std::move(handler);
    routes.push_back(std::move(entry));
}

void Router::use(Middleware mw)
{
    middlewares.push_back(std::move(mw));
}

HttpResponse Router::handle(HttpRequest &req) const
{
    // auto reqParts=splitPath(req.path);

    // for(const auto& route : routes)
    // {
    //     if(route.method!=req.method)
    //         continue;
        
    //     if(route.parts.size()!=reqParts.size())
    //         continue;

    //     bool match=true;

    //     req.params.clear();

    //     for(size_t i=0;i<route.parts.size();i++)
    //     {
    //         const auto& rp=route.parts[i];

    //         const auto& qp=reqParts[i];


    //         // 动态参数
    //         if(!rp.empty()&&rp[0]==':')
    //         {
    //             req.params[rp.substr(1)]=qp;
                
    //         }else if(rp!=qp)
    //         {
    //             match=false;
    //             break;
    //         }
    //     }

    //     if(match)
    //     {
    //         return route.handler(req);
    //     }
    // }
    // return HttpResponse::stock404();
    Context ctx;
    ctx.req=std::move(req);
    auto reqParts=splitPath(ctx.req.path);

    for(auto &mw:middlewares )
    {
        if(!mw(ctx))
        {

            return ctx.resp;
        }
    }
    // 路由匹配
    for(const auto& route : routes)
    {
        if(route.method!=ctx.req.method)
            continue;
        
        if(route.parts.size()!=reqParts.size())
            continue;

        bool match=true;

        ctx.params.clear();

        for(size_t i=0;i<route.parts.size();i++)
        {
            const auto& rp=route.parts[i];

            const auto& qp=reqParts[i];


            // 动态参数
            if(!rp.empty()&&rp[0]==':')
            {
                ctx.params[rp.substr(1)]=qp;
                
            }else if(rp!=qp)
            {
                match=false;
                break;
            }
        }

        if(match)
        {
            route.handler(ctx);
            return ctx.resp;
        }
    }
    return HttpResponse::stock404();
}


// 获得参数
std::string Context::param(const std::string &key)
{
    auto it=params.find(key);
    if(it==params.end())
    {
        return "";
    }
    return it->second;
}

// 查询参数
std::string Context::querry(const std::string &key)
{
    auto it=req.querryParams.find(key);
    // 防止自动插入一个数据进去
    if(it==req.querryParams.end())
    {
        return "";
    }
    return it->second;
}