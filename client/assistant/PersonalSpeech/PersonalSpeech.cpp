#include "PersonalSpeech.h"
#include "client/assistant/WindowsCloudSpeech/WindowsCloudSpeech.h"
#include "client/companion/BoundedPool/BoundedPool.h"
#include "client/companion/ProxyServer/ProxyServer.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include "backend/ai/speech/application/CloudSpeechPolicy/CloudSpeechPolicy.h"
#include "backend/ai/speech/application/WaveCodec/WaveCodec.h"
#include <atomic>
#include <cmath>
#include <thread>
namespace nebula::client {
namespace j=boost::json;namespace http=boost::beast::http;using namespace speech;
namespace{
std::string text(const j::object&o,const char*k,std::string fallback={}){const auto*v=o.if_contains(k);if(!v)return fallback;return std::string(v->as_string());}
SpeechProfile profile(const j::object&o){SpeechProfile p;const auto provider=text(o,"provider","local");if(provider!="local"&&provider!="azure"&&provider!="gemini")throw std::runtime_error("未知语音服务");p.provider=provider=="azure"?Provider::Azure:provider=="gemini"?Provider::Gemini:Provider::Local;
    p.voice=text(o,"voice","zf_001");p.style=text(o,"style",p.voice=="zm_009"?"narration-relaxed":"calm");p.region=text(o,"region","eastasia");if(auto*v=o.if_contains("speed"))p.speed=j::value_to<double>(*v);if(auto*v=o.if_contains("pitch"))p.pitchPercent=j::value_to<int>(*v);if(auto*v=o.if_contains("freeTierConfirmed"))p.freeTierConfirmed=v->as_bool();if(auto*v=o.if_contains("dataUseConsent"))p.dataUseConsent=v->as_bool();CloudSpeechPolicy::validate(p);return p;}
j::object json(const SpeechProfile&p){return{{"provider",providerName(p.provider)},{"voice",p.voice},{"style",p.style},{"region",p.region},{"speed",p.speed},{"pitch",p.pitchPercent},{"freeTierConfirmed",p.freeTierConfirmed},{"dataUseConsent",p.dataUseConsent}};}
ProxyRequest upstreamRequest(http::verb method,std::string path,j::value value={}){ProxyRequest r{method,std::move(path),11};if(!value.is_null()){r.set(http::field::content_type,"application/json");r.body()=j::serialize(value);}r.prepare_payload();return r;}
}
struct PersonalSpeech::Impl:ISpeechBudget{
    SqliteStore&store;std::string account;UpstreamTransport&upstream;WindowsCloudSpeech cloud;CloudSpeechPolicy policy;
    struct Preview{std::string state="pending",provider,reason,error;std::vector<unsigned char>wave;};
    std::mutex mutex;std::map<std::string,Preview>previews;std::atomic_bool stopping{};BoundedPool pool{1,2};
    Impl(SqliteStore&s,std::string a,UpstreamTransport&u):store(s),account(std::move(a)),upstream(u),cloud([this](Provider p){return key(p);}),policy(cloud,*this){
        store.execute("CREATE TABLE IF NOT EXISTS personal_speech(key TEXT PRIMARY KEY,value TEXT NOT NULL)");
        store.execute("CREATE TABLE IF NOT EXISTS personal_speech_usage(provider TEXT,bucket TEXT,units INTEGER NOT NULL,PRIMARY KEY(provider,bucket))");
    }
    ~Impl(){stopping=true;pool.shutdown();}
    std::string get(const std::string&key){auto rows=store.query("SELECT value FROM personal_speech WHERE key=?",{key});return rows.empty()?"":*rows[0][0];}
    std::string key(Provider p){auto value=get("key:"+providerName(p));return value.empty()?"":unprotectCredential(value,account);}
    SpeechProfile current(){auto value=get("profile");return value.empty()?SpeechProfile{}:profile(j::parse(value).as_object());}
    bool reserve(Provider p,std::size_t count)override{bool accepted=false;store.transact([&]{const auto name=providerName(p);const auto bucket=store.query("SELECT strftime('month:%Y-%m','now'),strftime('minute:%Y-%m-%d-%H-%M','now')")[0];const auto month=*bucket[0],minute=*bucket[1];store.execute("DELETE FROM personal_speech_usage WHERE bucket LIKE 'minute:%' AND bucket<?",{minute});
        std::size_t used=0,calls=0;for(auto&r:store.query("SELECT bucket,units FROM personal_speech_usage WHERE provider=? AND bucket IN (?,?)",{name,month,minute}))(*r[0]==month?used:calls)=std::stoull(*r[1]);const auto cap=p==Provider::Azure?500000u:100000u;if(count>cap||used>cap-count||calls>=(p==Provider::Azure?20u:3u))return;
        for(auto&e:std::vector<std::pair<std::string,std::size_t>>{{month,count},{minute,1}})store.execute("INSERT INTO personal_speech_usage VALUES(?,?,?) ON CONFLICT(provider,bucket) DO UPDATE SET units=units+excluded.units",{name,e.first,std::to_string(e.second)});accepted=true;});return accepted;}
    void save(const j::object&o){const auto p=profile(o);store.transact([&]{store.execute("INSERT INTO personal_speech VALUES('profile',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",{j::serialize(json(p))});
        if(auto*v=o.if_contains("apiKey")){auto value=std::string(v->as_string());if(value.size()>512||value.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("Key 格式无效");if(!value.empty()){if(p.provider==Provider::Local)throw std::runtime_error("本地语音不需要 Key");store.execute("INSERT INTO personal_speech VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",{"key:"+providerName(p.provider),protectCredential(value,account)});}}
        if(auto*v=o.if_contains("removeKey");v&&v->as_bool())store.execute("DELETE FROM personal_speech WHERE key=?",{"key:"+providerName(p.provider)});
    });}
    std::vector<unsigned char>local(const BoundServerSession&id,const std::string&voice,double speed){
        auto response=upstream.requestBound(id,upstreamRequest(http::verb::post,"/api/me/speech/preview",j::object{{"voice",voice},{"speed",std::to_string(speed)}}));if(response.result_int()!=200)throw std::runtime_error("本地语音暂不可用");const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
        while(!stopping&&std::chrono::steady_clock::now()<deadline){if(upstream.session().epoch!=id.epoch)throw std::runtime_error("账号已改变");auto state=upstream.requestBound(id,upstreamRequest(http::verb::get,"/api/me/speech/preview/"+voice));if(state.result_int()!=200)throw std::runtime_error("试听状态不可用");auto value=j::parse(state.body());const auto status=std::string(value.at("state").as_string());if(status=="failed")throw std::runtime_error("本地语音合成失败");if(status=="ready"){if(!value.as_object().contains("speed")||std::abs(boost::json::value_to<double>(value.at("speed"))-speed)>.001)throw std::runtime_error("试听设置已改变，请重新试听");const auto url=std::string(value.at("url").as_string());if(!url.starts_with("/api/me/speech/audio/"))throw std::runtime_error("试听地址无效");auto audio=upstream.requestBound(id,upstreamRequest(http::verb::get,url));std::vector<unsigned char>wave(audio.body().begin(),audio.body().end());if(audio.result_int()!=200||!WaveCodec::valid(wave))throw std::runtime_error("试听音频无效");return wave;}std::this_thread::sleep_for(std::chrono::milliseconds(250));}throw std::runtime_error("试听已取消或超时");
    }
};
PersonalSpeech::PersonalSpeech(SqliteStore&s,std::string name,UpstreamTransport&u):impl_(std::make_unique<Impl>(s,std::move(name),u)){}
PersonalSpeech::~PersonalSpeech()=default;
ProxyResponse PersonalSpeech::handle(const std::string&route,const ProxyRequest&r,const j::object&o,const BoundServerSession&id){auto&s=*impl_;
    if(route=="/speech/settings"&&r.method()==http::verb::get){auto result=json(s.current());result["azureKeyConfigured"]=s.cloud.configured(Provider::Azure);result["geminiKeyConfigured"]=s.cloud.configured(Provider::Gemini);result["budgetKind"]="application-estimate-not-provider-balance";j::array usage;for(auto&row:s.store.query("SELECT provider,bucket,units FROM personal_speech_usage ORDER BY bucket DESC LIMIT 12"))usage.push_back(j::object{{"provider",*row[0]},{"bucket",*row[1]},{"units",*row[2]}});result["usage"]=std::move(usage);return localJson(200,result);}
    if(route=="/speech/settings"&&r.method()==http::verb::patch){s.save(o);return localJson(200,{{"ok",true}});}
    if(route=="/speech/preview"&&r.method()==http::verb::post){auto p=s.current();const auto voice=p.voice;std::lock_guard lock(s.mutex);if(auto found=s.previews.find(voice);found!=s.previews.end()&&found->second.state=="pending")return localJson(202,{{"voice",voice},{"state","pending"}});s.previews[voice]={};
        if(!s.pool.submit([&s,p,id]{Impl::Preview result;try{auto output=s.policy.generate(p,"欢迎来到星云，神树将为你播报新的故事。",[&]{return s.local(id,p.voice,p.speed);});result.wave=std::move(output.wave);result.provider=providerName(output.provider);result.reason=output.fallbackReason;result.state="ready";}catch(...){result.state="failed";result.error="试听失败，请检查服务器语音 Worker 或云设置";}std::lock_guard guard(s.mutex);s.previews[p.voice]=std::move(result);})){s.previews.erase(voice);return localJson(503,{{"message","试听队列已满"}});}return localJson(202,{{"voice",voice},{"state","pending"}});}
    const std::string prefix="/speech/preview/";if(route.starts_with(prefix)&&r.method()==http::verb::get){auto voice=route.substr(prefix.size());const bool audio=voice.ends_with("/audio");if(audio)voice.resize(voice.size()-6);std::lock_guard lock(s.mutex);auto found=s.previews.find(voice);if(found==s.previews.end())return localJson(404,{{"message","没有此试听"}});auto&p=found->second;
        if(audio&&p.state=="ready"){ProxyResponse response{http::status::ok,11};response.set(http::field::content_type,"audio/wav");response.set(http::field::cache_control,"no-store");response.body().assign(p.wave.begin(),p.wave.end());response.prepare_payload();return response;}
        return localJson(200,{{"state",p.state},{"actualProvider",p.provider},{"fallbackReason",p.reason},{"error",p.error}});}
    return localJson(404,{{"message","没有此语音操作"}});
}
}
