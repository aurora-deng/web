// ==============================================================================
// 文件名：RequestContext.cpp
// 职责比喻：这是"档案袋"上两个"查询窗口"的实现。
//   RequestContext.h 定义了档案袋的结构，本文件实现两个取参数的方法：
//   param() 取路径参数（路由匹配出的，如 /user/:id 的 id）；
//   querry() 取查询参数（URL ? 后的键值对，如 ?id=123 的 id）。
//   两者找不到都返回空串，不抛异常，让业务侧用空串判断"有没有"。
//
// 关键技术点（初学者重点理解）：
//   1. 【找不到返回空串】而不是抛异常或断言，因为"参数没传"在 HTTP 里是常态（可选参数），
//      业务侧用 .empty() 判断即可，简单友好。
//   2. 【避免 operator[] 副作用】用 find 而非 []，因为 [] 会在找不到时插入一个空值，
//      污染参数表，且并发下可能有数据竞争问题。
// ==============================================================================
#include "RequestContext.h"

/**
 * @brief 取路径参数
 * @param key 参数名（路由定义的占位符，如 :id 的 "id"）
 * @return 参数值；不存在返回空串
 * @note 用 find 而非 operator[]，避免找不到时往 params 里插入空值污染参数表。
 */
std::string RequestContext::param(const std::string &key)
{
    auto it=params.find(key);
    if(it==params.end())
    {
        return "";
    }
    return it->second;
}

/**
 * @brief 取查询参数
 * @param key 参数名（URL ? 后的键）
 * @return 参数值；不存在返回空串
 * @note 同样用 find 避免 operator[] 的自动插入副作用。
 */
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
