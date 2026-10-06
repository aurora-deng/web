#include "server/phase11/ai/OllamaStreamDecoder/OllamaStreamDecoder.h"

#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace webserver::phase11
{
namespace
{

void appendCodePoint(std::string &out, std::uint32_t codePoint)
{
    if (codePoint <= 0x7fU)
        out.push_back(static_cast<char>(codePoint));
    else if (codePoint <= 0x7ffU)
    {
        out.push_back(static_cast<char>(0xc0U | (codePoint >> 6U)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    }
    else if (codePoint <= 0xffffU)
    {
        if (codePoint >= 0xd800U && codePoint <= 0xdfffU)
            throw std::invalid_argument("unpaired JSON surrogate");
        out.push_back(static_cast<char>(0xe0U | (codePoint >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
    }
    else if (codePoint <= 0x10ffffU)
    {
        out.push_back(static_cast<char>(0xf0U | (codePoint >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (codePoint & 0x3fU)));
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

class JsonCursor final
{
public:
    explicit JsonCursor(std::string_view input) : input_(input) {}

    OllamaStreamEvent parseEvent()
    {
        OllamaStreamEvent event;
        bool sawContent = false;
        bool sawDone = false;
        bool sawError = false;

        skipWhitespace();
        expect('{');
        skipWhitespace();
        if (!consume('}'))
        {
            while (true)
            {
                const auto key = string();
                skipWhitespace();
                expect(':');
                skipWhitespace();
                if (key == "message")
                    parseMessage(event, sawContent);
                else if (key == "done")
                {
                    if (sawDone)
                        throw std::invalid_argument("duplicate Ollama done field");
                    event.done = boolean();
                    sawDone = true;
                }
                else if (key == "error")
                {
                    if (sawError)
                        throw std::invalid_argument("duplicate Ollama error field");
                    event.error = string();
                    sawError = true;
                }
                else
                    skipValue(0);

                skipWhitespace();
                if (consume('}'))
                    break;
                expect(',');
                skipWhitespace();
            }
        }
        skipWhitespace();
        if (position_ != input_.size())
            throw std::invalid_argument("trailing data after Ollama JSON object");
        if (!sawContent && !sawDone && !sawError)
            throw std::invalid_argument("Ollama event contains no recognized field");
        return event;
    }

private:
    void parseMessage(OllamaStreamEvent &event, bool &sawContent)
    {
        expect('{');
        skipWhitespace();
        if (consume('}'))
            return;
        while (true)
        {
            const auto key = string();
            skipWhitespace();
            expect(':');
            skipWhitespace();
            if (key == "content")
            {
                if (sawContent)
                    throw std::invalid_argument("duplicate Ollama content field");
                event.content = string();
                sawContent = true;
            }
            else
                skipValue(1);
            skipWhitespace();
            if (consume('}'))
                return;
            expect(',');
            skipWhitespace();
        }
    }

    void skipValue(unsigned depth)
    {
        if (depth > 32)
            throw std::invalid_argument("Ollama JSON nesting is too deep");
        skipWhitespace();
        if (position_ >= input_.size())
            throw std::invalid_argument("truncated Ollama JSON value");
        const char current = input_[position_];
        if (current == '"')
        {
            (void)string();
            return;
        }
        if (current == '{')
        {
            ++position_;
            skipWhitespace();
            if (consume('}')) return;
            while (true)
            {
                (void)string();
                skipWhitespace();
                expect(':');
                skipValue(depth + 1);
                skipWhitespace();
                if (consume('}')) return;
                expect(',');
                skipWhitespace();
            }
        }
        if (current == '[')
        {
            ++position_;
            skipWhitespace();
            if (consume(']')) return;
            while (true)
            {
                skipValue(depth + 1);
                skipWhitespace();
                if (consume(']')) return;
                expect(',');
                skipWhitespace();
            }
        }
        if (match("true") || match("false") || match("null"))
            return;
        number();
    }

    bool boolean()
    {
        if (match("true")) return true;
        if (match("false")) return false;
        throw std::invalid_argument("Ollama done field is not boolean");
    }

    void number()
    {
        const auto start = position_;
        consume('-');
        if (consume('0'))
        {
            if (position_ < input_.size() && std::isdigit(
                    static_cast<unsigned char>(input_[position_])))
                throw std::invalid_argument("invalid JSON number");
        }
        else
        {
            if (position_ >= input_.size() || !std::isdigit(
                    static_cast<unsigned char>(input_[position_])))
                throw std::invalid_argument("invalid JSON value");
            while (position_ < input_.size() && std::isdigit(
                       static_cast<unsigned char>(input_[position_])))
                ++position_;
        }
        if (consume('.'))
        {
            if (position_ >= input_.size() || !std::isdigit(
                    static_cast<unsigned char>(input_[position_])))
                throw std::invalid_argument("invalid JSON fraction");
            while (position_ < input_.size() && std::isdigit(
                       static_cast<unsigned char>(input_[position_])))
                ++position_;
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E'))
        {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-'))
                ++position_;
            if (position_ >= input_.size() || !std::isdigit(
                    static_cast<unsigned char>(input_[position_])))
                throw std::invalid_argument("invalid JSON exponent");
            while (position_ < input_.size() && std::isdigit(
                       static_cast<unsigned char>(input_[position_])))
                ++position_;
        }
        if (position_ == start)
            throw std::invalid_argument("invalid JSON value");
    }

    std::string string()
    {
        expect('"');
        std::string result;
        while (position_ < input_.size())
        {
            const auto current = static_cast<unsigned char>(input_[position_++]);
            if (current == '"') return result;
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
                if (codePoint >= 0xd800U && codePoint <= 0xdbffU)
                {
                    if (position_ + 2 > input_.size() ||
                        input_[position_] != '\\' || input_[position_ + 1] != 'u')
                        throw std::invalid_argument("unpaired JSON high surrogate");
                    position_ += 2;
                    const auto low = hex4();
                    if (low < 0xdc00U || low > 0xdfffU)
                        throw std::invalid_argument("invalid JSON low surrogate");
                    codePoint = 0x10000U + ((codePoint - 0xd800U) << 10U) +
                                (low - 0xdc00U);
                }
                appendCodePoint(result, codePoint);
                break;
            }
            default: throw std::invalid_argument("invalid JSON escape");
            }
        }
        throw std::invalid_argument("unterminated JSON string");
    }

    std::uint32_t hex4()
    {
        if (position_ + 4 > input_.size())
            throw std::invalid_argument("truncated JSON unicode escape");
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index)
        {
            const int digit = hexDigit(input_[position_++]);
            if (digit < 0)
                throw std::invalid_argument("invalid JSON unicode escape");
            result = (result << 4U) | static_cast<std::uint32_t>(digit);
        }
        return result;
    }

    void skipWhitespace()
    {
        while (position_ < input_.size() && std::isspace(
                   static_cast<unsigned char>(input_[position_])))
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
            throw std::invalid_argument("malformed Ollama JSON object");
    }

    bool match(std::string_view literal)
    {
        if (input_.substr(position_, literal.size()) != literal)
            return false;
        position_ += literal.size();
        return true;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

OllamaStreamEvent parseLine(std::string_view line)
{
    return JsonCursor(line).parseEvent();
}

} // namespace

OllamaStreamDecoder::OllamaStreamDecoder(std::size_t maxLineBytes)
    : maxLineBytes_(maxLineBytes)
{
    if (maxLineBytes_ == 0)
        throw std::invalid_argument("Ollama maximum line size must be positive");
}

std::vector<OllamaStreamEvent> OllamaStreamDecoder::feed(std::string_view bytes)
{
    pending_.append(bytes);
    return consumeCompleteLines(false);
}

std::vector<OllamaStreamEvent> OllamaStreamDecoder::finish()
{
    return consumeCompleteLines(true);
}

std::vector<OllamaStreamEvent>
OllamaStreamDecoder::consumeCompleteLines(bool flushTail)
{
    std::vector<OllamaStreamEvent> events;
    while (true)
    {
        auto delimiter = pending_.find('\n');
        if (delimiter == std::string::npos)
        {
            if (!flushTail)
                break;
            delimiter = pending_.size();
        }
        if (delimiter > maxLineBytes_)
            throw std::invalid_argument("Ollama response line exceeds configured limit");

        std::string line = pending_.substr(0, delimiter);
        pending_.erase(0, delimiter + (delimiter < pending_.size() ? 1 : 0));
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto first = line.find_first_not_of(" \t\r");
        if (first != std::string::npos)
        {
            if (terminal_)
                throw std::invalid_argument("Ollama sent data after terminal event");
            auto event = parseLine(std::string_view(line).substr(first));
            if (!event.error.empty() || event.done)
                terminal_ = true;
            events.push_back(std::move(event));
        }
        if (delimiter == line.size() && pending_.empty() && flushTail)
            break;
    }
    if (pending_.size() > maxLineBytes_)
        throw std::invalid_argument("Ollama response line exceeds configured limit");
    return events;
}

} // namespace webserver::phase11
