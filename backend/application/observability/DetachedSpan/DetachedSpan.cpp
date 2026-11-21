#include "DetachedSpan.h"
#include "backend/application/observability/BusinessOperation/BusinessOperation.h"
#include <array>
#include <openssl/rand.h>

namespace webserver::telemetry {
namespace {
std::string identifier(unsigned size){
    std::array<unsigned char,16> data{};
    if(RAND_bytes(data.data(),static_cast<int>(size))!=1)return {};
    constexpr std::string_view hex="0123456789abcdef";
    std::string result;result.reserve(size*2);
    for(unsigned i=0;i<size;++i){result+=hex[data[i]>>4];result+=hex[data[i]&15];}
    return result;
}
}
DetachedSpan::DetachedSpan(Series series) noexcept {
    try{
        name_=operationName(series);if(name_.empty()||!tracingEnabled())return;
        auto parent=Span::context();trace_=std::move(parent.first);parent_=std::move(parent.second);
        if(trace_.empty())trace_=identifier(16);
        id_=identifier(8);if(trace_.empty()||id_.empty()){recordTraceDrop();return;}
        unixStart_=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        started_=Clock::now();finished_.store(false,std::memory_order_release);
    }catch(...){recordTraceDrop();}
}
void DetachedSpan::finish(int status) noexcept {
    if(finished_.exchange(true,std::memory_order_acq_rel))return;
    try{
        if(status<0||status>2)status=2;
        const auto duration=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started_).count();
        std::string json="{\"traceId\":\""+trace_+"\",\"spanId\":\""+id_+"\",\"name\":\""+std::string(name_)+
            "\",\"kind\":1,\"startTimeUnixNano\":\""+std::to_string(unixStart_)+"\",\"endTimeUnixNano\":\""+
            std::to_string(unixStart_+duration)+"\",\"status\":{\"code\":"+std::to_string(status)+"}";
        if(!parent_.empty())json+=",\"parentSpanId\":\""+parent_+"\"";
        json+='}';submitSpan(std::move(json),status==2);
    }catch(...){recordTraceDrop();}
}
DetachedSpan::~DetachedSpan(){finish(0);}
}
