#pragma once
#include <filesystem>
#include <string>
#include <string_view>
namespace nebula::client {
std::string randomToken();
std::string sha256(std::string_view value);
std::string protectCredential(std::string_view value,std::string_view accountNamespace);
std::string unprotectCredential(std::string_view value,std::string_view accountNamespace);
void createPrivateDirectory(const std::filesystem::path&path);
}
