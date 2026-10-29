#include "LoopbackPolicy.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
namespace nebula::client {
namespace {
bool equal(std::string_view a,std::string_view b){if(a.size()!=b.size())return false;unsigned char difference=0;for(std::size_t i=0;i<a.size();i++)difference|=static_cast<unsigned char>(a[i]^b[i]);return difference==0;}
}
LoopbackPolicy::LoopbackPolicy(unsigned short port):port_(port),launch_(randomToken()),session_(randomToken()),csrf_(randomToken()){}
std::string LoopbackPolicy::origin()const{return "http://localhost:"+std::to_string(port_);}
bool LoopbackPolicy::validHost(const ProxyRequest&r)const{return r[client_http::field::host]=="localhost:"+std::to_string(port_);}
bool LoopbackPolicy::validOrigin(const ProxyRequest&r,bool required)const{
    const auto origin=r[client_http::field::origin];if(origin.empty())return !required;
    return origin==this->origin()&&r["Sec-Fetch-Site"]!="cross-site";
}
bool LoopbackPolicy::authenticated(const ProxyRequest&r)const{
    if(std::chrono::steady_clock::now()>=expires_)return false;
    std::string_view cookies=r[client_http::field::cookie];bool found=false;
    while(!cookies.empty()){const auto end=cookies.find(';');auto part=cookies.substr(0,end);while(!part.empty()&&part.front()==' ')part.remove_prefix(1);
        if(part.starts_with("nebula_local=")){if(found||!equal(part.substr(13),session_))return false;found=true;}
        if(end==std::string_view::npos)break;cookies.remove_prefix(end+1);
    }return found;
}
bool LoopbackPolicy::csrf(const ProxyRequest&r)const{return equal(std::string_view(r["X-Local-CSRF"]),csrf_);}
std::string LoopbackPolicy::bootstrap(const std::string&token){if(launch_.empty()||!equal(token,launch_))return{};launch_.clear();expires_=std::chrono::steady_clock::now()+std::chrono::hours(24);return "nebula_local="+session_+"; Path=/; HttpOnly; SameSite=Strict; Max-Age=86400";}
}
