#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
namespace nebula::speech {
class WaveCodec final{
public:
    static std::vector<unsigned char>base64(std::string_view value);
    static std::vector<unsigned char>pcm24k(std::vector<unsigned char>pcm);
    static bool valid(const std::vector<unsigned char>&wave);
};
}
