#pragma once
#include "client/companion/UpstreamTransport/UpstreamTransport.h"
#include <filesystem>
#include <mutex>
namespace nebula::client {
/** 只保存DPAPI密封后的会话Cookie；不保存登录密码。 */
class CredentialVault final{
public:
    CredentialVault(std::filesystem::path root,std::string site);
    std::string load();void save(const BoundServerSession&session);
private:std::filesystem::path file_;std::string site_;std::mutex mutex_;
};
}
