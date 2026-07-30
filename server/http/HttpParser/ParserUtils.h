#pragma once
#ifndef PARSER_UTILS_H
#define PARSER_UTILS_H

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>

// 作为统一的工具栏文件

// 可以使用命名空间来对其进行详细空间管束，效果和类差不多，不过实现不了多态

// 解析器各阶段的大小上限：在分配大对象前拒绝异常输入，约束单连接资源占用。
// HttpParser.h 中的 MAX_* 静态常量引用这些值，保持对外接口不变。
inline constexpr size_t kMaxRequestLineBytes = 8 * 1024;
inline constexpr size_t kMaxHeaderBytes = 64 * 1024;
inline constexpr size_t kMaxBodyBytes = 1024 * 1024;


// 统一小写
inline std::string toLower(std::string s)
{
    for (char &c : s)
        c = std::tolower((unsigned char)c);
    return s;
}

// 作用：去掉前后空格与制表符
inline std::string trim(const std::string &s)
{
    // 去掉前导空格
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
        start++;
    // 去掉后导空格
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r'))
        end--;
    return s.substr(start, end - start);
}

// RFC 7230 tchar。请求方法和首部名只能由这些字节组成；拒绝空白、控制字符和分隔符，
// 可防止代理与源站对 "Content-Length :" 等畸形字段产生不同解释。
inline bool isHttpToken(const std::string &value)
{
    if (value.empty())
        return false;
    for (unsigned char c : value)
    {
        if (std::isalnum(c))
            continue;
        switch (c)
        {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            continue;
        default:
            return false;
        }
    }
    return true;
}

#endif
