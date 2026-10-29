#pragma once
#include <string>
#include <vector>
namespace nebula::speech {
/** 有界的朗读文本归一与语义标点切分，不声称能替代有情绪能力的模型。 */
class SpeechText final{
public:static std::string normalize(std::string text);static std::vector<std::string>segments(std::string text);
};
}
