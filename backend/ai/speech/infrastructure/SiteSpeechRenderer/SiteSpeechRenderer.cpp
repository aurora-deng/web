#include "backend/ai/speech/infrastructure/SpeechCredentialStore/SpeechCredentialStore.h"
#include "SiteSpeechRenderer.h"
#include "backend/ai/speech/application/WaveCodec/WaveCodec.h"
#include "backend/application/transport/FlatJson/FlatJson.h"
#include <fstream>
#include <cstdlib>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
namespace webserver::function3 {
using namespace nebula::speech;
namespace {std::string quote(std::string_view s){return phase11::transport::quoteJson(s);}std::string digest(const std::vector<unsigned char>&bytes){unsigned char raw[32];unsigned n{};if(!EVP_Digest(bytes.data(),bytes.size(),raw,&n,EVP_sha256(),nullptr))throw std::runtime_error("speech cache digest failed");std::string out;for(auto c:raw){out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}return out;}}
SiteSpeechRenderer::SiteSpeechRenderer(SqlContext&s,SpeechProcessRenderer&local):sql_(s),local_(local),cloud_(key),policy_(cloud_,*this){}
namespace {
std::filesystem::path credentialPath(Provider p){
    if(p!=Provider::Azure&&p!=Provider::Gemini)throw std::invalid_argument("cloud provider required");
    const auto*configured=std::getenv(p==Provider::Azure?"WEB_SPEECH_AZURE_KEY_FILE":"WEB_SPEECH_GEMINI_KEY_FILE");
    return configured&&*configured?std::filesystem::path(configured):std::filesystem::path("/home/pikachu/phase11-data/function3/speech-secrets")/(providerName(p)+".key");
}
}
std::string SiteSpeechRenderer::key(Provider p){try{return SpeechCredentialStore::read(credentialPath(p));}catch(...){return{};}}
void SiteSpeechRenderer::configureCredential(Provider p,const std::string&value,bool remove,unsigned long long actor){
    if(p!=Provider::Azure&&p!=Provider::Gemini)throw BusinessError(Failure::Invalid,"cloud provider required");
    if(!remove&&(value.empty()||value.size()>512||value.find_first_of("\r\n")!=std::string::npos))throw BusinessError(Failure::Invalid,"invalid speech key");
    // Audit only the provider and operation. No credentials or request payloads go to SQL.
    sql_.query("INSERT INTO site_audit(actor_id,action,resource,reason) VALUES($1,'speech.credential.requested',$2,$3)",{std::to_string(actor),"speech:"+providerName(p),remove?"remove":"replace"});
    try{SpeechCredentialStore::write(credentialPath(p),value,remove);}catch(const std::invalid_argument&){throw BusinessError(Failure::Invalid,"invalid speech key");}catch(...){throw BusinessError(Failure::Busy,"cannot save private speech key; check configured file permissions");}
    sql_.query("INSERT INTO site_audit(actor_id,action,resource,reason) VALUES($1,'speech.credential.saved',$2,$3)",{std::to_string(actor),"speech:"+providerName(p),remove?"removed":"replaced"});
}
SpeechProfile SiteSpeechRenderer::profile(const std::string&voice)const{
    auto rows=sql_.query("SELECT provider,style,region,speed::text,pitch::text,free_confirmed,data_consent FROM speech_profiles WHERE voice=$1",{voice});
    if(rows.empty())throw BusinessError(Failure::Invalid,"unsupported speech preset");auto&r=rows[0];SpeechProfile p;p.voice=voice;p.provider=r[0]=="azure"?Provider::Azure:r[0]=="gemini"?Provider::Gemini:Provider::Local;p.style=r[1];p.region=r[2];p.speed=std::stod(r[3]);p.pitchPercent=std::stoi(r[4]);p.freeTierConfirmed=r[5]=="t";p.dataUseConsent=r[6]=="t";return p;
}
void SiteSpeechRenderer::configure(const SpeechProfile&p,unsigned long long actor){
    try{CloudSpeechPolicy::validate(p);}catch(const std::invalid_argument&e){throw BusinessError(Failure::Invalid,e.what());}
    sql_.transact([&]{sql_.query("UPDATE speech_profiles SET provider=$2,style=$3,region=$4,speed=$5,pitch=$6,free_confirmed=$7,data_consent=$8,updated_at=CURRENT_TIMESTAMP WHERE voice=$1",{p.voice,providerName(p.provider),p.style,p.region,std::to_string(p.speed),std::to_string(p.pitchPercent),p.freeTierConfirmed?"true":"false",p.dataUseConsent?"true":"false"});
        sql_.query("INSERT INTO site_audit(actor_id,action,resource,reason) VALUES($1,'speech.configure',$2,$3)",{std::to_string(actor),"speech:"+p.voice,providerName(p.provider)+":"+p.style});});
}
bool SiteSpeechRenderer::reserve(Provider p,std::size_t count){bool accepted=false;sql_.transact([&]{
    sql_.query("SELECT pg_advisory_xact_lock(3700400)");const auto name=providerName(p);
    const auto buckets=sql_.query("SELECT 'month:'||to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC','YYYY-MM'),'minute:'||to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC','YYYY-MM-DD-HH24-MI')")[0];
    sql_.query("DELETE FROM speech_usage WHERE bucket LIKE 'minute:%' AND bucket<$1",{buckets[1]});
    auto rows=sql_.query("SELECT bucket,units FROM speech_usage WHERE provider=$1 AND bucket IN ($2,$3)",{name,buckets[0],buckets[1]});std::size_t month=0,minute=0;for(auto&r:rows)(r[0]==buckets[0]?month:minute)=std::stoull(r[1]);
    const auto cap=p==Provider::Azure?500000u:100000u;const auto rpm=p==Provider::Azure?20u:3u;if(count>cap||month>cap-count||minute>=rpm)return;
    for(const auto&entry:std::vector<std::pair<std::string,std::size_t>>{{buckets[0],count},{buckets[1],1}})sql_.query("INSERT INTO speech_usage VALUES($1,$2,$3) ON CONFLICT(provider,bucket) DO UPDATE SET units=speech_usage.units+EXCLUDED.units",{name,entry.first,std::to_string(entry.second)});accepted=true;
});return accepted;}
std::string SiteSpeechRenderer::render(const SpeechJob&job){
    std::lock_guard single(rendering_); // 两份公告与试听共用单个模型/音频缓存写入预算。
    auto p=profile(job.voice);if(job.speed>0)p.speed=job.speed;
    const auto descriptor="cloud-speech-v1:"+providerName(p.provider)+":"+p.model+":"+p.voice+":"+p.style+":"+p.region+":"+std::to_string(p.speed)+":"+std::to_string(p.pitchPercent)+":"+(p.freeTierConfirmed?"yes":"no")+":"+(p.dataUseConsent?"yes":"no")+":"+job.text;
    const auto cloudKey=digest({descriptor.begin(),descriptor.end()});const auto cached=local_.path(cloudKey);
    if(job.announcement!=0&&p.provider!=Provider::Local&&std::filesystem::is_regular_file(cached)&&std::filesystem::file_size(cached)<=6*1024*1024&&std::filesystem::last_write_time(cached)>std::filesystem::file_time_type::clock::now()-std::chrono::hours(24)){
        std::ifstream file(cached,std::ios::binary);std::vector<unsigned char>wave(std::istreambuf_iterator<char>(file),{});
        if(WaveCodec::valid(wave)){std::lock_guard lock(mutex_);lastProvider_=providerName(p.provider);lastFallback_="";return cloudKey;}
    }
    std::string localKey;auto result=policy_.generate(p,job.text,[&]{auto localJob=job;localJob.speed=p.speed;localKey=local_.render(localJob);std::ifstream file(local_.path(localKey),std::ios::binary);return std::vector<unsigned char>(std::istreambuf_iterator<char>(file),{});});
    {std::lock_guard lock(mutex_);lastProvider_=providerName(result.provider);lastFallback_=result.fallbackReason;}
    if(!localKey.empty())return localKey;
    local_.storeWave(cloudKey,result.wave);return cloudKey;
}
std::string SiteSpeechRenderer::status()const{std::lock_guard lock(mutex_);return "{\"actualProvider\":"+quote(lastProvider_)+",\"fallbackReason\":"+quote(lastFallback_)+",\"azureKeyConfigured\":"+(cloud_.configured(Provider::Azure)?"true":"false")+",\"geminiKeyConfigured\":"+(cloud_.configured(Provider::Gemini)?"true":"false")+",\"budgetKind\":\"application-estimate-not-provider-balance\"}";}
}
