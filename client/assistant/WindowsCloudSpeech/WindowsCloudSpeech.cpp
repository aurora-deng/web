#include "WindowsCloudSpeech.h"
#include "backend/ai/speech/application/SpeechWire/SpeechWire.h"
#include <windows.h>
#include <winhttp.h>
#include <boost/json.hpp>
#include <memory>
namespace nebula::client {
using namespace speech;
namespace {struct Close{void operator()(void*p)const{if(p)WinHttpCloseHandle(p);}};using Handle=std::unique_ptr<void,Close>;std::wstring wide(const std::string&s){return {s.begin(),s.end()};}}
bool WindowsCloudSpeech::configured(Provider p)const{try{return !keys_(p).empty();}catch(...){return false;}}
CloudAudio WindowsCloudSpeech::synthesize(const SpeechProfile&p,const std::string&text){
    auto r=SpeechWire::request(p,text);const auto key=keys_(p.provider);if(key.empty()||key.size()>512||key.find_first_of("\r\n")!=std::string::npos)return{};
    Handle session(WinHttpOpen(L"NebulaCompanion/Phase40",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));if(!session)return{};
    WinHttpSetTimeouts(session.get(),3000,3000,5000,15000);Handle connection(WinHttpConnect(session.get(),wide(r.host).c_str(),443,0));if(!connection)return{};
    Handle request(WinHttpOpenRequest(connection.get(),L"POST",wide(r.target).c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));if(!request)return{};
    DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;if(!WinHttpSetOption(request.get(),WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)))return{};
    // 不设置忽略证书错误，不使用 Linux 学习证书的进程内信任。Key 不进入 URL。
    auto headers="Content-Type: "+r.contentType+"\r\n"+(p.provider==Provider::Azure?"Ocp-Apim-Subscription-Key: ":"x-goog-api-key: ")+key+"\r\n";
    if(p.provider==Provider::Azure)headers+="X-Microsoft-OutputFormat: riff-24khz-16bit-mono-pcm\r\n";
    auto h=wide(headers);const auto deadline=GetTickCount64()+15000;
    if(!WinHttpSendRequest(request.get(),h.c_str(),static_cast<DWORD>(h.size()),r.body.data(),static_cast<DWORD>(r.body.size()),static_cast<DWORD>(r.body.size()),0)||!WinHttpReceiveResponse(request.get(),nullptr))return{};
    DWORD status{},bytes=sizeof(status);if(!WinHttpQueryHeaders(request.get(),WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&bytes,WINHTTP_NO_HEADER_INDEX))return{};
    if(status!=200)return{static_cast<int>(status),{},"provider-rejected"};std::string body;char block[16384];
    for(;;){const auto now=GetTickCount64();if(now>=deadline)return{};DWORD timeout=static_cast<DWORD>(deadline-now);WinHttpSetOption(request.get(),WINHTTP_OPTION_RECEIVE_TIMEOUT,&timeout,sizeof(timeout));DWORD n{};if(!WinHttpReadData(request.get(),block,sizeof(block),&n))return{};if(!n)break;if(body.size()+n>4*1024*1024)return{502,{},"response-limit"};body.append(block,n);}
    if(p.provider==Provider::Azure)return{200,{body.begin(),body.end()},{}};
    try{const auto parsed=boost::json::parse(body);const auto&candidates=parsed.at("candidates").as_array();if(candidates.size()!=1)return{502,{},"invalid-candidates"};
        for(const auto&part:candidates.at(0).at("content").at("parts").as_array())if(const auto*audio=part.as_object().if_contains("inlineData"))return{200,SpeechWire::geminiAudio(std::string(audio->at("data").as_string()),std::string(audio->at("mimeType").as_string())),{}};
    }catch(...){return{502,{},"invalid-audio"};}return{502,{},"missing-audio"};
}
}
