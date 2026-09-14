// ==============================================================================
// 文件名：ParserUtils.h
// 职责比喻：这是 HTTP 解析器的"工具箱"——分拣员们共用的"剪刀、尺子、标签纸"。
//   三个子解析器（RequestLineParser/HeaderParser/BodyParser）都会用到这些通用工具：
//   - kMax*Bytes 常量：三把"尺子"，限定请求行/头部/正文的最大长度，超长直接拒收；
//   - toLower：把字符串统一转小写，HTTP 头部名大小写不敏感；
//   - trim：去掉首尾空白，处理 "Key: Value" 中 Value 的多余空格；
//   - isHttpToken：校验字段名是否合法（RFC 7230 tchar），挡住畸形输入。
//
// 关键技术点（初学者重点理解）：
//   1. 【大小上限的意义】没有上限的话，攻击者发一个无限长的请求行/头部就能撑爆内存。
//      kMax*Bytes 在分配大对象前就拒绝，是防 DoS 的第一道闸门。
//   2. 【大小写不敏感】HTTP 规定头部名大小写等价，统一转小写后存储/比较，避免 "Content-Type"
//      和 "content-type" 被当成两个不同的头。
//   3. 【RFC 7230 tchar 校验】请求方法和头部名只能由特定字符组成，拒绝空白和控制字符，
//      可防止代理与源站对畸形字段产生不同解释（请求走私攻击的常见入口）。
// ==============================================================================
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
inline constexpr size_t kMaxRequestLineBytes = 8 * 1024;   // 请求行上限 8KB
inline constexpr size_t kMaxHeaderBytes = 64 * 1024;       // 头部上限 64KB
inline constexpr size_t kMaxBodyBytes = 1024 * 1024;       // 正文上限 1MB


/**
 * @brief 把字符串所有字符转成小写
 * @param s 待转换字符串（按值传入，原字符串不变）
 * @return 全小写的新字符串
 * @note HTTP 头部名大小写不敏感，统一小写后存储，避免重复头和匹配遗漏。
 */
inline std::string toLower(std::string s)
{
    for (char &c : s)
        c = std::tolower((unsigned char)c);
    return s;
}

/**
 * @brief 去掉字符串首尾的空格、制表符和回车
 * @param s 待处理的字符串
 * @return 去掉首尾空白后的子串
 * @note 用于处理 "Content-Type:   text/html" 中 Value 前后的多余空格。
 */
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

/**
 * @brief 校验字符串是否符合 RFC 7230 的 tchar 规则（合法的 token）
 * @param value 待校验字符串
 * @return true 合法；false 含非法字符或为空
 * @note 请求方法（GET/POST）和头部字段名必须是 token：只能由字母数字和少数符号组成，
 *       不允许空白、控制字符、分隔符。拒绝畸形输入可防请求走私。
 */
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
