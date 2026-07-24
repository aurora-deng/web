#include "RequestContext.h"

std::string RequestContext::param(const std::string &key)
{
    auto it=params.find(key);
    if(it==params.end())
    {
        return "";
    }
    return it->second;
}

std::string RequestContext::querry(const std::string &key)
{
     auto it=request.querryParams.find(key);
    // 防止自动插入一个数据进去
    if(it==request.querryParams.end())
    {
        return "";
    }
    return it->second;
}
