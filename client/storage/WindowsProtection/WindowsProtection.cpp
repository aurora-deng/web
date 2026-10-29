#include "WindowsProtection.h"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <sddl.h>
#include <vector>
#include <stdexcept>
namespace nebula::client {
namespace {
std::string hex(const unsigned char*data,std::size_t size){const char*digits="0123456789abcdef";std::string result;result.reserve(size*2);for(std::size_t i=0;i<size;i++){result+=digits[data[i]>>4];result+=digits[data[i]&15];}return result;}
std::vector<unsigned char>unhex(std::string_view value){if(value.size()%2||value.size()>65536)throw std::runtime_error("Invalid credential envelope");std::vector<unsigned char>result;auto n=[](char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("Invalid credential envelope");};for(std::size_t i=0;i<value.size();i+=2)result.push_back(static_cast<unsigned char>((n(value[i])<<4)|n(value[i+1])));return result;}
}
std::string randomToken(){unsigned char value[32];if(BCryptGenRandom(nullptr,value,sizeof(value),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)throw std::runtime_error("Secure random generator failed");return hex(value,sizeof(value));}
std::string sha256(std::string_view value){BCRYPT_ALG_HANDLE algorithm{};if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)!=0)throw std::runtime_error("SHA256 unavailable");BCRYPT_HASH_HANDLE hash{};unsigned char digest[32];const auto status=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0);if(status!=0){BCryptCloseAlgorithmProvider(algorithm,0);throw std::runtime_error("SHA256 initialization failed");}const auto updated=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),static_cast<ULONG>(value.size()),0);const auto finished=BCryptFinishHash(hash,digest,sizeof(digest),0);BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);if(updated!=0||finished!=0)throw std::runtime_error("SHA256 failed");return hex(digest,sizeof(digest));}
std::string protectCredential(std::string_view value,std::string_view account) {
    if(value.size()>16384||account.empty())throw std::runtime_error("Invalid credential size or namespace");
    DATA_BLOB input{static_cast<DWORD>(value.size()),reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))},entropy{static_cast<DWORD>(account.size()),reinterpret_cast<BYTE*>(const_cast<char*>(account.data()))},output{};
    if(!CryptProtectData(&input,L"Nebula account credential",&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))throw std::runtime_error("DPAPI protection failed");
    auto result=hex(output.pbData,output.cbData);SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);return result;
}
std::string unprotectCredential(std::string_view value,std::string_view account) {
    auto bytes=unhex(value);DATA_BLOB input{static_cast<DWORD>(bytes.size()),bytes.data()},entropy{static_cast<DWORD>(account.size()),reinterpret_cast<BYTE*>(const_cast<char*>(account.data()))},output{};
    if(!CryptUnprotectData(&input,nullptr,&entropy,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))throw std::runtime_error("DPAPI credential belongs to another identity or is damaged");
    std::string result(reinterpret_cast<char*>(output.pbData),output.cbData);SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);return result;
}
void createPrivateDirectory(const std::filesystem::path&path) {
    if(!path.is_absolute())throw std::runtime_error("Private storage requires an explicit absolute path");
    // 只给当前 Windows 用户和 SYSTEM 权限；不改变父目录/全盘权限。
    HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))throw std::runtime_error("Cannot read Windows identity");
    DWORD size{};GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<BYTE>buffer(size);
    if(!GetTokenInformation(token,TokenUser,buffer.data(),size,&size)){CloseHandle(token);throw std::runtime_error("Cannot read Windows SID");}CloseHandle(token);
    LPWSTR sid{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid))throw std::runtime_error("Cannot encode Windows SID");
    const std::wstring sddl=L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;"+std::wstring(sid)+L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor{};if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))throw std::runtime_error("Cannot prepare private directory ACL");
    std::filesystem::create_directories(path);
    const auto attributes=GetFileAttributesW(path.c_str());
    const bool safe=attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
    const bool applied=safe&&SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,descriptor);
    LocalFree(descriptor);if(!applied)throw std::runtime_error("Cannot restrict private directory permissions");
}
}
