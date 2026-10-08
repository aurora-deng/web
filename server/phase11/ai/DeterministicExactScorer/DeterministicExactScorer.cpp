#include "server/phase11/ai/DeterministicExactScorer/DeterministicExactScorer.h"

#include <cctype>
#include <stdexcept>

namespace webserver::phase11
{

std::string DeterministicExactScorer::normalize(std::string_view value)
{
    std::string normalized;
    normalized.reserve(value.size());
    bool pendingSpace = false;
    for (const unsigned char character : value)
    {
        if (character < 128U && std::isspace(character))
        {
            pendingSpace = !normalized.empty();
            continue;
        }
        if (pendingSpace)
        {
            normalized.push_back(' ');
            pendingSpace = false;
        }
        normalized.push_back(static_cast<char>(
            character < 128U ? std::tolower(character) : character));
    }
    return normalized;
}

DeterministicScore DeterministicExactScorer::score(
    std::string_view expected, std::string_view actual) const
{
    const auto normalizedExpected = normalize(expected);
    if (normalizedExpected.empty())
        throw std::invalid_argument(
            "normalized-exact-v1 requires a non-empty expected answer");
    const auto normalizedActual = normalize(actual);
    const bool passed = normalizedActual == normalizedExpected;
    return {passed ? 1.0 : 0.0, passed,
            passed ? "normalized exact answer matched"
                   : "normalized exact answer differed"};
}

} // namespace webserver::phase11
