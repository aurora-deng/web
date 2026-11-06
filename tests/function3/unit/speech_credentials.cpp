#include "backend/ai/speech/infrastructure/SpeechCredentialStore/SpeechCredentialStore.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using webserver::function3::SpeechCredentialStore;
void check(bool ok,const char*why){if(!ok)throw std::runtime_error(why);}
int main(){
    char pattern[]="/tmp/nebula-speech-XXXXXX";const auto*created=mkdtemp(pattern);if(!created)return 1;
    const std::filesystem::path root(created),key=root/"key";
    try{
        SpeechCredentialStore::write(key,"test-placeholder-one");check(SpeechCredentialStore::read(key)=="test-placeholder-one","saved key missing");
        struct stat info{};check(!stat(key.c_str(),&info)&&!(info.st_mode&0077),"key permissions exposed");
        SpeechCredentialStore::write(key,"test-placeholder-two");check(SpeechCredentialStore::read(key)=="test-placeholder-two","atomic replacement missing");
        chmod(key.c_str(),0644);check(SpeechCredentialStore::read(key).empty(),"public file read");
        bool rejected=false;try{SpeechCredentialStore::write(key,"replacement");}catch(...){rejected=true;}check(rejected,"public file overwritten silently");chmod(key.c_str(),0600);
        const auto link=root/"link";symlink(key.c_str(),link.c_str());check(SpeechCredentialStore::read(link).empty(),"symlink followed");
        rejected=false;try{SpeechCredentialStore::write(link,"replacement");}catch(...){rejected=true;}check(rejected,"symlink overwritten");
        const auto pipe=root/"pipe";mkfifo(pipe.c_str(),0600);check(SpeechCredentialStore::read(pipe).empty(),"FIFO treated as credential");
        rejected=false;try{SpeechCredentialStore::write(key,"invalid\nkey");}catch(...){rejected=true;}check(rejected,"header injection allowed");
        SpeechCredentialStore::write(key,"",true);check(!std::filesystem::exists(key),"remove failed");
        SpeechCredentialStore::write(key,"",true);check(SpeechCredentialStore::read(key).empty(),"duplicate removal not idempotent");
        std::filesystem::remove_all(root);std::cout<<"PASS private credential atomic replacement, owner permissions, symlink/FIFO rejection and idempotent removal\n";
    }catch(const std::exception&e){std::filesystem::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}
}
