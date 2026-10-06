#include "server/phase11/transport/FlatJson/FlatJson.h"

#include <cctype>
#include <cstdint>
#include <stdexcept>

namespace webserver::phase11::transport
{
namespace
{

void appendCodePoint(std::string &out, std::uint32_t codePoint)
{
    if (codePoint <= 0x7FU)
        out.push_back(static_cast<char>(codePoint));
    else if (codePoint <= 0x7FFU)
    {
        out.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else if (codePoint <= 0xFFFFU)
    {
        if (codePoint >= 0xD800U && codePoint <= 0xDFFFU)
            throw std::invalid_argument("unpaired JSON surrogate");
        out.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else if (codePoint <= 0x10FFFFU)
    {
        out.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
    else
        throw std::invalid_argument("JSON code point is out of range");
}

int hexDigit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

class Parser final
{
public:
    explicit Parser(std::string_view input) : input_(input) {}

    FlatStringObject parse()
    {
        skipWhitespace();
        expect('{');
        FlatStringObject result;
        skipWhitespace();
        if (consume('}'))
        {
            finish();
            return result;
        }

        while (true)
        {
            skipWhitespace();
            auto key = string();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            auto value = string();
            if (!result.emplace(std::move(key), std::move(value)).second)
                throw std::invalid_argument("duplicate JSON field");
            skipWhitespace();
            if (consume('}'))
                break;
            expect(',');
        }
        finish();
        return result;
    }

private:
    void skipWhitespace()
    {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_])))
            ++position_;
    }

    bool consume(char wanted)
    {
        if (position_ >= input_.size() || input_[position_] != wanted)
            return false;
        ++position_;
        return true;
    }

    void expect(char wanted)
    {
        if (!consume(wanted))
            throw std::invalid_argument("malformed JSON object");
    }

    std::uint32_t hex4()
    {
        if (position_ + 4 > input_.size())
            throw std::invalid_argument("truncated JSON unicode escape");
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index)
        {
            const int digit = hexDigit(input_[position_++]);
            if (digit < 0)
                throw std::invalid_argument("invalid JSON unicode escape");
            value = (value << 4U) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    std::string string()
    {
        expect('"');
        std::string result;
        while (position_ < input_.size())
        {
            const auto current = static_cast<unsigned char>(input_[position_++]);
            if (current == '"')
                return result;
            if (current < 0x20U)
                throw std::invalid_argument("unescaped JSON control character");
            if (current != '\\')
            {
                result.push_back(static_cast<char>(current));
                continue;
            }
            if (position_ >= input_.size())
                throw std::invalid_argument("truncated JSON escape");
            switch (input_[position_++])
            {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u':
            {
                auto codePoint = hex4();
                if (codePoint >= 0xD800U && codePoint <= 0xDBFFU)
                {
                    if (position_ + 2 > input_.size() ||
                        input_[position_] != '\\' || input_[position_ + 1] != 'u')
                        throw std::invalid_argument("unpaired JSON high surrogate");
                    position_ += 2;
                    const auto low = hex4();
                    if (low < 0xDC00U || low > 0xDFFFU)
                        throw std::invalid_argument("invalid JSON low surrogate");
                    codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) +
                                (low - 0xDC00U);
                }
                appendCodePoint(result, codePoint);
                break;
            }
            default:
                throw std::invalid_argument("invalid JSON escape");
            }
        }
        throw std::invalid_argument("unterminated JSON string");
    }

    void finish()
    {
        skipWhitespace();
        if (position_ != input_.size())
            throw std::invalid_argument("trailing data after JSON object");
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

} // namespace

FlatStringObject parseFlatStringObject(std::string_view json)
{
    return Parser(json).parse();
}

std::string quoteJson(std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const unsigned char current : value)
    {
        switch (current)
        {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (current < 0x20U)
            {
                result += "\\u00";
                result.push_back(hex[current >> 4U]);
                result.push_back(hex[current & 0x0FU]);
            }
            else
                result.push_back(static_cast<char>(current));
        }
    }
    result.push_back('"');
    return result;
}

} // namespace webserver::phase11::transport
