#include "SpeechProcessRenderer.h"
#include <cmath>
#include "backend/application/modules/shared/infrastructure/ProcessRunner/ProcessRunner.h"
#include <algorithm>
#include <fstream>
#include <openssl/evp.h>
#include <memory>
#include "backend/ai/speech/application/WaveCodec/WaveCodec.h"
namespace webserver::function3 {
namespace {
std::string digest(std::string value){unsigned char bytes[32];unsigned int length{};if(EVP_Digest(value.data(),value.size(),bytes,&length,EVP_sha256(),nullptr)!=1)throw std::runtime_error("digest failure");std::string result;for(unsigned char c:bytes){result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}return result;}
bool keyValid(std::string_view key){return key.size()==64&&std::all_of(key.begin(),key.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
}
std::string SpeechProcessRenderer::path(std::string key)const{if(!keyValid(key))throw BusinessError(Failure::NotFound,"audio cache key invalid");return(config_.cache/(key+".wav")).string();}
void SpeechProcessRenderer::verifyModel()const{
    // 清单仅由已校验官方压缩包的安装脚本生成。启动时流式复核，防止文件损坏后悄悄换模型。
    std::ifstream manifest(config_.model/"MODEL_FILES.sha256");
    if(!manifest)throw std::runtime_error("TTS model checksum manifest missing; run approved local preparation script");
    std::string line;std::size_t count=0;bool model=false,voices=false;
    while(std::getline(manifest,line)){
        if(line.size()<67||line.substr(64,2)!="  ")throw std::runtime_error("invalid TTS checksum manifest line");
        const auto expected=line.substr(0,64),name=line.substr(66);
        std::filesystem::path relative(name);
        if(!keyValid(expected)||relative.is_absolute()||name.find("..")!=std::string::npos)throw std::runtime_error("invalid TTS checksum manifest");
        std::ifstream file(config_.model/relative,std::ios::binary);if(!file)throw std::runtime_error("TTS model resource missing");
        std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);
        if(!ctx||EVP_DigestInit_ex(ctx.get(),EVP_sha256(),nullptr)!=1)throw std::runtime_error("TTS checksum init failed");
        char buffer[65536];while(file.read(buffer,sizeof(buffer))||file.gcount())if(EVP_DigestUpdate(ctx.get(),buffer,file.gcount())!=1)throw std::runtime_error("TTS checksum failed");
        unsigned char bytes[32];unsigned int size{};if(file.bad()||EVP_DigestFinal_ex(ctx.get(),bytes,&size)!=1)throw std::runtime_error("TTS model read failed");
        std::string actual;for(auto c:bytes){actual+="0123456789abcdef"[c>>4];actual+="0123456789abcdef"[c&15];}
        if(actual!=expected)throw std::runtime_error("TTS model checksum mismatch: "+name);
        model|=name=="model.int8.onnx";voices|=name=="voices.bin";++count;
    }
    if(!model||!voices||count<7)throw std::runtime_error("incomplete TTS checksum manifest");
}
void SpeechProcessRenderer::quota(){
    std::vector<std::filesystem::directory_entry> entries;std::uint64_t total=0;
    for(auto&e:std::filesystem::directory_iterator(config_.cache))if(!e.is_symlink()&&e.is_regular_file()){
        const auto name=e.path().filename().string();
        const bool wave=name.size()==68&&name.ends_with(".wav")&&keyValid(name.substr(0,64));
        const bool transient=((name.size()==76&&name.ends_with(".wav.partial"))||(name.size()==68&&name.ends_with(".txt")))&&keyValid(name.substr(0,64));
        if(transient&&e.last_write_time()<std::filesystem::file_time_type::clock::now()-std::chrono::minutes(2)){std::filesystem::remove(e.path());continue;}
        if(wave||transient){entries.push_back(e);total+=e.file_size();}
    }
    std::sort(entries.begin(),entries.end(),[](auto&a,auto&b){return a.last_write_time()<b.last_write_time();});
    const auto expired=std::filesystem::file_time_type::clock::now()-std::chrono::hours(24);
    for(auto&e:entries){if(total<=240*1024*1024&&e.last_write_time()>=expired)break;total-=e.file_size();std::filesystem::remove(e.path());}
}
std::string SpeechProcessRenderer::render(const SpeechJob&job){
    std::lock_guard lock(rendering_); // 公告与后台试听共享单个模型进程预算。
    if(cancelled_.load(std::memory_order_relaxed))throw BusinessError(Failure::Busy,"speech worker stopped");
    if(!std::filesystem::is_regular_file(config_.executable))throw BusinessError(Failure::Busy,"local TTS worker is not configured");
    if(job.text.empty()||job.text.size()>4096||(job.voice!="zf_001"&&job.voice!="zm_009"))throw BusinessError(Failure::Invalid,"invalid speech job");
    std::filesystem::create_directories(config_.cache);quota();
    const auto speed=job.speed>0?job.speed:(job.voice=="zf_001"?.95:.94);
    if(!std::isfinite(speed)||speed<.75||speed>1.25)throw BusinessError(Failure::Invalid,"speech speed exceeds range");
    const auto key=digest(job.text+"\n"+config_.modelDigest+"\n"+job.voice+"\nprosody-v2:"+std::to_string(speed));
    const auto output=path(key);if(std::filesystem::exists(output))return key;
    const auto input=config_.cache/(key+".txt");const auto temporary=output+".partial";
    {std::ofstream file(input);file<<job.text;}
    auto result=runProcess({config_.executable.string(),config_.model.string(),job.voice,input.string(),temporary,std::to_string(speed)},std::chrono::seconds(90),3072,
        {"PATH=/usr/bin:/bin","LANG=C.UTF-8","LD_LIBRARY_PATH="+(config_.engine/"lib").string()},&cancelled_);
    std::filesystem::remove(input);
    if(result.status||result.timedOut||!std::filesystem::exists(temporary)){std::error_code error;std::filesystem::remove(temporary,error);throw BusinessError(Failure::Busy,"speech worker failed");}
    std::filesystem::rename(temporary,output);quota();return key;
}
void SpeechProcessRenderer::storeWave(const std::string&key,const std::vector<unsigned char>&wave){
    if(!nebula::speech::WaveCodec::valid(wave))throw BusinessError(Failure::Invalid,"invalid cloud speech audio");
    std::lock_guard lock(rendering_);std::filesystem::create_directories(config_.cache);quota();const auto output=path(key);
    if(std::filesystem::exists(output))return;const auto temporary=output+".partial";
    {std::ofstream file(temporary,std::ios::binary);file.write(reinterpret_cast<const char*>(wave.data()),wave.size());if(!file)throw BusinessError(Failure::Busy,"speech audio cache failed");}
    std::filesystem::rename(temporary,output);quota();
}
}
