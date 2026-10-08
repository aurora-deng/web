#pragma once

#include <string>
#include <string_view>

namespace webserver::phase11
{

struct DeterministicScore
{
    double score = 0.0;
    bool passed = false;
    std::string detail;
};

/**
 * 无外部模型的可复现评分器。
 *
 * 它只折叠 Unicode 文本中的 ASCII 空白，并把 ASCII 大写转为小写；中文等
 * 非 ASCII 字节保持原样。适合“请精确回答某句话”类回归题，不适合开放问答。
 */
class DeterministicExactScorer final
{
public:
    [[nodiscard]] static constexpr std::string_view id() noexcept
    {
        return "normalized-exact-v1";
    }

    [[nodiscard]] DeterministicScore score(
        std::string_view expected, std::string_view actual) const;

private:
    [[nodiscard]] static std::string normalize(std::string_view value);
};

} // namespace webserver::phase11
