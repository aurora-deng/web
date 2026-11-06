#pragma once
#include "backend/application/modules/shared/transport/BusinessHttp/BusinessHttp.h"
#include "backend/ai/speech/application/WaveCodec/WaveCodec.h"
#include <fstream>
#include <algorithm>
#include <vector>

namespace webserver::function3::http {
/** Short speech is bounded to one minute. A memory response also works over H2;
 * the HTTP/1 sendfile response is not supported by the frozen H2 adapter.
 * Range chunks stay below its existing 1 MiB response cap. */
inline void speechAudio(RequestContext&ctx,const std::string&path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw BusinessError(Failure::NotFound,"speech audio cache expired; generate preview again");
    const auto size=file.tellg();
    if(size<44||size>6*1024*1024)throw BusinessError(Failure::Busy,"invalid speech audio size");
    std::vector<unsigned char>bytes(static_cast<std::size_t>(size));file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size())||!nebula::speech::WaveCodec::valid(bytes))
        throw BusinessError(Failure::Busy,"speech synthesis produced invalid WAV audio");
    ctx.response->status=200;ctx.response->statusText="OK";ctx.response->headers.erase("Content-Range");
    std::size_t first=0,last=bytes.size()-1;bool partial=false;
    const auto range=ctx.request.headers.find("range");
    auto parse=[](std::string_view value,std::size_t&result){const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);return !value.empty()&&parsed.ec==std::errc{}&&parsed.ptr==value.data()+value.size();};
    if(range!=ctx.request.headers.end()){
        const std::string_view value(range->second);const auto dash=value.find('-');bool valid=value.starts_with("bytes=")&&dash!=std::string_view::npos&&value.find(',')==std::string_view::npos;
        if(valid&&dash==6){std::size_t suffix{};valid=parse(value.substr(7),suffix)&&suffix>0;if(valid)first=suffix>=bytes.size()?0:bytes.size()-suffix;}
        else if(valid){valid=parse(value.substr(6,dash-6),first);if(dash+1<value.size())valid=valid&&parse(value.substr(dash+1),last);}
        if(!valid||first>=bytes.size()||last<first){ctx.response->status=416;ctx.response->statusText="Range Not Satisfiable";ctx.response->text("");ctx.response->setHeader("Content-Range","bytes */"+std::to_string(bytes.size()));ctx.response->setHeader("Cache-Control","private,no-store");return;}
        last=std::min(last,bytes.size()-1);partial=true;
    }
    if(ctx.request.version.starts_with("HTTP/2")&&((!partial&&bytes.size()>1024*1024)||(partial&&last-first+1>512*1024)))
        throw BusinessError(Failure::Invalid,"use byte Range requests of at most 512 KiB for large HTTP/2 speech audio");
    if(partial){
        ctx.response->status=206;ctx.response->statusText="Partial Content";
        ctx.response->setHeader("Content-Range","bytes "+std::to_string(first)+"-"+std::to_string(last)+"/"+std::to_string(bytes.size()));
    }
    ctx.response->text(std::string_view(reinterpret_cast<const char*>(bytes.data()+first),last-first+1));
    ctx.response->setHeader("Content-Type","audio/wav");
    ctx.response->setHeader("Accept-Ranges","bytes");
    ctx.response->setHeader("Cache-Control","private,no-store");
    ctx.response->setHeader("X-Content-Type-Options","nosniff");
}
}
