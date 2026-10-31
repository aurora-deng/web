#pragma once
#include "backend/application/config/RuntimeConfig/RuntimeConfig.h"
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <sys/stat.h>
namespace webserver::bootstrap {
inline bool flag(const char* name){return config::RuntimeConfig::flag(name);}
inline std::size_t size(const char* name,std::size_t fallback,std::size_t low,std::size_t high){return config::RuntimeConfig::size(name,fallback,low,high);}
/** Read only the configured secret file; never log its contents. */
inline std::string privateHexKey(const char* path){
    if(!path || !*path)throw std::invalid_argument("WEB_PHASE11_TOKEN_KEY_FILE is required");
    struct stat info{};
    if(::stat(path,&info)!=0 || !S_ISREG(info.st_mode) || (info.st_mode&(S_IRWXG|S_IRWXO)))
        throw std::invalid_argument("WEB_PHASE11_TOKEN_KEY_FILE must be a private regular file");
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::invalid_argument("cannot read WEB_PHASE11_TOKEN_KEY_FILE");
    std::string key{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    while(!key.empty()&&(key.back()=='\n'||key.back()=='\r'))key.pop_back();
    if(key.size()!=64)throw std::invalid_argument("WEB_PHASE11_TOKEN_KEY_FILE must contain 64 hexadecimal characters");
    return key;
}
}
