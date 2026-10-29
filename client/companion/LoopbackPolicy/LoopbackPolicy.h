#pragma once
#include "client/companion/UpstreamTransport/UpstreamTransport.h"
#include <string>
#include <chrono>
namespace nebula::client {
/** 入口验证在任何代理/私人资料读取之前执行，防止 DNS rebinding 与跨站请求。 */
class LoopbackPolicy final {
public:
    explicit LoopbackPolicy(unsigned short port);
    std::string origin()const;
    bool validHost(const ProxyRequest&request)const;
    bool validOrigin(const ProxyRequest&request,bool required)const;
    bool authenticated(const ProxyRequest&request)const;
    bool csrf(const ProxyRequest&request)const;
    std::string bootstrap(const std::string&token);
    const std::string&launchToken()const noexcept{return launch_;}
    const std::string&csrfToken()const noexcept{return csrf_;}
private:unsigned short port_;std::string launch_,session_,csrf_;
    std::chrono::steady_clock::time_point expires_{};
};
}
