#include "client/companion/LoopbackPolicy/LoopbackPolicy.h"
#include <iostream>
using namespace nebula::client;
void check(bool value,const char*message){if(!value)throw std::runtime_error(message);}
int main(){try{
    LoopbackPolicy policy(18091);ProxyRequest request{client_http::verb::post,"/api/local/assistant/memories",11};
    request.set(client_http::field::host,"evil.example:18091");request.set(client_http::field::origin,"http://evil.example");
    check(!policy.validHost(request)&&!policy.validOrigin(request,true),"DNS rebinding / hostile origin accepted");
    request.set(client_http::field::host,"localhost:18091");request.set(client_http::field::origin,policy.origin());
    check(policy.validHost(request)&&policy.validOrigin(request,true),"local origin rejected");check(!policy.authenticated(request),"missing local session accepted");
    const auto launch=policy.launchToken();check(policy.bootstrap("wrong").empty(),"wrong bootstrap accepted");
    const auto cookie=policy.bootstrap(launch);check(!cookie.empty()&&policy.bootstrap(launch).empty(),"bootstrap replay accepted");
    request.set(client_http::field::cookie,cookie.substr(0,cookie.find(';')));check(policy.authenticated(request),"local session missing");
    request.set("X-Local-CSRF",policy.csrfToken());check(policy.csrf(request),"local CSRF missing");
    request.set(client_http::field::origin,"http://attacker.example");check(!policy.validOrigin(request,true),"valid cookie bypasses hostile Origin");
    request.erase(client_http::field::origin);check(!policy.validOrigin(request,true),"write accepted without Origin");
    std::cout<<"PASS local Host/Origin/session/CSRF and one-use bootstrap protection\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
