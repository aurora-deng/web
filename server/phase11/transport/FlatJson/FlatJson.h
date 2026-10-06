#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace webserver::phase11::transport
{

using FlatStringObject = std::unordered_map<std::string, std::string>;

/**
 * 解析只含字符串值的一层 JSON 对象。
 *
 * Phase 11 的认证请求只有少量文本字段，先用这个边界清晰的小解析器，避免在尚未获准
 * 安装依赖时偷偷引入 JSON 库。它仍会完整检查转义、Unicode surrogate pair、重复键和
 * 尾随垃圾；数组、嵌套对象、数字、布尔和 null 会被明确拒绝。
 */
[[nodiscard]] FlatStringObject parseFlatStringObject(std::string_view json);

/** 把任意 UTF-8 字节串安全放入 JSON 字符串；控制字符会被转义。 */
[[nodiscard]] std::string quoteJson(std::string_view value);

} // namespace webserver::phase11::transport
