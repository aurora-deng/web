#pragma once
#include <filesystem>
#include <string>

namespace webserver::function3 {
/** Site keys live in owner-only files, never in speech_profiles or an API response. */
class SpeechCredentialStore final {
public:
    static std::string read(const std::filesystem::path&);
    static void write(const std::filesystem::path&,const std::string&key,bool remove=false);
};
}
