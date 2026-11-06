#include "SpeechCredentialStore.h"
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace webserver::function3 {
namespace {
struct Descriptor {int fd;~Descriptor(){if(fd>=0)close(fd);}};
bool privateFile(int fd){struct stat info{};return fd>=0&&!fstat(fd,&info)&&S_ISREG(info.st_mode)&&info.st_uid==geteuid()&&!(info.st_mode&0077);}
}
std::string SpeechCredentialStore::read(const std::filesystem::path&path) {
    Descriptor file{open(path.c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC)};if(!privateFile(file.fd))return{};
    struct stat info{};if(fstat(file.fd,&info)||info.st_size<=0||info.st_size>513)return{};
    char bytes[514];const auto n=::read(file.fd,bytes,sizeof(bytes));if(n<=0||n>513)return{};
    std::string key(bytes,n);while(!key.empty()&&(key.back()=='\n'||key.back()=='\r'))key.pop_back();
    if(key.empty()||key.size()>512||std::any_of(key.begin(),key.end(),[](unsigned char c){return c<=32||c==127;}))return{};
    return key;
}
void SpeechCredentialStore::write(const std::filesystem::path&path,const std::string&key,bool remove) {
    if(!path.is_absolute()||path.filename().empty()||path.filename()=="."||path.filename()=="..")throw std::runtime_error("invalid private credential path");
    if(!remove&&(key.empty()||key.size()>512||std::any_of(key.begin(),key.end(),[](unsigned char c){return c<=32||c==127;})))throw std::invalid_argument("invalid speech key");
    const auto parent=path.parent_path();
    if(!std::filesystem::exists(parent)){
        // The pre-existing data root is not chmod'ed. Only our new private leaf is created.
        if(::mkdir(parent.c_str(),0700)&&!std::filesystem::exists(parent))throw std::runtime_error("cannot create private speech directory");
    }
    Descriptor directory{open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC)};
    struct stat info{};if(directory.fd<0||fstat(directory.fd,&info)||info.st_uid!=geteuid()||(info.st_mode&0022))throw std::runtime_error("speech credential directory must be owned and not writable by others");
    Descriptor existing{openat(directory.fd,path.filename().c_str(),O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC)};
    if(existing.fd<0&&std::filesystem::symlink_status(path).type()!=std::filesystem::file_type::not_found)throw std::runtime_error("unsafe existing credential file");
    if(existing.fd>=0&&!privateFile(existing.fd))throw std::runtime_error("speech key file must be owner-only and regular");
    if(remove){if(existing.fd>=0&&unlinkat(directory.fd,path.filename().c_str(),0))throw std::runtime_error("cannot remove speech key");fsync(directory.fd);return;}
    auto pattern=(parent/(path.filename().string()+".pending-XXXXXX")).string();std::vector<char>name(pattern.begin(),pattern.end());name.push_back(0);
    Descriptor temporary{mkstemp(name.data())};if(temporary.fd<0)throw std::runtime_error("cannot create private speech key");
    try{
        std::size_t offset=0;while(offset<key.size()){const auto count=::write(temporary.fd,key.data()+offset,key.size()-offset);if(count<=0)throw std::runtime_error("speech key write failed");offset+=count;}
        if(fsync(temporary.fd)||renameat(directory.fd,std::filesystem::path(name.data()).filename().c_str(),directory.fd,path.filename().c_str())||fsync(directory.fd))throw std::runtime_error("speech key save failed");
    }catch(...){unlink(name.data());throw;}
}
}
