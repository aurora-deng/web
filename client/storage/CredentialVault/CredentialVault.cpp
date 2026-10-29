#include "CredentialVault.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include <windows.h>
#include <fstream>
namespace nebula::client {
CredentialVault::CredentialVault(std::filesystem::path root,std::string site):site_(std::move(site)){createPrivateDirectory(root);file_=root/(sha256(site_)+".session");}
std::string CredentialVault::load(){std::lock_guard lock(mutex_);if(!std::filesystem::exists(file_))return{};if(std::filesystem::file_size(file_)>65536)throw std::runtime_error("本机凭据文件过大");std::ifstream input(file_,std::ios::binary);return unprotectCredential(std::string(std::istreambuf_iterator<char>(input),{}),site_);}
void CredentialVault::save(const BoundServerSession&session){std::lock_guard lock(mutex_);if(session.cookie.empty()){std::error_code ignored;std::filesystem::remove(file_,ignored);return;}
    const auto envelope=protectCredential(session.cookie,site_);const auto pending=std::filesystem::path(file_.wstring()+L".pending");std::ofstream out(pending,std::ios::binary|std::ios::trunc);out.write(envelope.data(),envelope.size());out.close();if(!out||!MoveFileExW(pending.c_str(),file_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("无法保存本机密封凭据");
}
}
