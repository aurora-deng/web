#pragma once
#include <array>
#include <cstdint>
#include <string_view>
namespace webserver::function3 {
/** Published limits are the same constants enforced before upload and before publishing. */
struct TreeContentPolicy {
    static constexpr std::uint64_t imageBytes=5ULL*1024*1024, voiceBytes=10ULL*1024*1024;
    static constexpr int images=6,voices=1,voiceMilliseconds=60000;
    static constexpr std::array<std::string_view,4> imageTypes{"image/png","image/jpeg","image/gif","image/webp"};
    static constexpr std::array<std::string_view,5> voiceTypes{"audio/wav","audio/x-wav","audio/mpeg","audio/ogg","audio/webm"};
};
}
