#include "CurlCloudSpeech.h"
#include "backend/ai/speech/application/SpeechWire/SpeechWire.h"
#include <curl/curl.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <memory>
namespace webserver::function3 {
using namespace nebula::speech;
namespace {
struct Body{std::string text;};
size_t receive(char*p,size_t size,size_t count,void*context){auto&body=*static_cast<Body*>(context);if(size&&count>4*1024*1024/size)return 0;auto n=size*count;if(body.text.size()+n>4*1024*1024)return 0;body.text.append(p,n);return n;}
const google::protobuf::Value&field(const google::protobuf::Struct&s,const char*name){return s.fields().at(name);}
}
bool CurlCloudSpeech::configured(Provider p)const{try{return !keys_(p).empty();}catch(...){return false;}}
CloudAudio CurlCloudSpeech::synthesize(const SpeechProfile&p,const std::string&text){
    const auto request=SpeechWire::request(p,text);const auto key=keys_(p.provider);
    if(key.empty()||key.size()>512||key.find_first_of("\r\n")!=std::string::npos)return {0,{},"missing-or-invalid-key"};
    static const auto initialized=curl_global_init(CURL_GLOBAL_DEFAULT);if(initialized!=CURLE_OK)return{};
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)>curl(curl_easy_init(),curl_easy_cleanup);if(!curl)return{};
    curl_slist*raw=nullptr;raw=curl_slist_append(raw,("Content-Type: "+request.contentType).c_str());
    raw=curl_slist_append(raw,((p.provider==Provider::Azure?"Ocp-Apim-Subscription-Key: ":"x-goog-api-key: ")+key).c_str());
    if(p.provider==Provider::Azure)raw=curl_slist_append(raw,"X-Microsoft-OutputFormat: riff-24khz-16bit-mono-pcm");
    std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)>headers(raw,curl_slist_free_all);Body body;
    const auto url="https://"+request.host+request.target;
    curl_easy_setopt(curl.get(),CURLOPT_URL,url.c_str());curl_easy_setopt(curl.get(),CURLOPT_HTTPHEADER,headers.get());
    curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDS,request.body.data());curl_easy_setopt(curl.get(),CURLOPT_POSTFIELDSIZE_LARGE,static_cast<curl_off_t>(request.body.size()));
    curl_easy_setopt(curl.get(),CURLOPT_CONNECTTIMEOUT_MS,3000L);curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT_MS,15000L);curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl.get(),CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl.get(),CURLOPT_SSL_VERIFYHOST,2L);
    curl_easy_setopt(curl.get(),CURLOPT_PROTOCOLS_STR,"https");curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,receive);curl_easy_setopt(curl.get(),CURLOPT_WRITEDATA,&body);
    if(curl_easy_perform(curl.get())!=CURLE_OK)return{};long status{};curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status);
    if(status!=200)return{static_cast<int>(status),{},"provider-rejected"};
    if(p.provider==Provider::Azure)return{200,{body.text.begin(),body.text.end()},{}};
    try{google::protobuf::Struct parsed;if(!google::protobuf::util::JsonStringToMessage(body.text,&parsed).ok())return{502,{},"invalid-json"};
        const auto&candidates=field(parsed,"candidates").list_value();if(candidates.values_size()!=1)return{502,{},"invalid-candidates"};
        const auto&parts=field(field(candidates.values(0).struct_value(),"content").struct_value(),"parts").list_value();
        for(const auto&part:parts.values()){const auto&fields=part.struct_value().fields();const auto found=fields.find("inlineData");if(found!=fields.end()){const auto&audio=found->second.struct_value();return{200,SpeechWire::geminiAudio(field(audio,"data").string_value(),field(audio,"mimeType").string_value()),{}};}}
    }catch(...){return{502,{},"invalid-audio"};}return{502,{},"missing-audio"};
}
}
