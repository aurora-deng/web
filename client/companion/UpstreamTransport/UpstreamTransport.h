#pragma once
#include "client/companion/SChannelStream/SChannelStream.h"
#include "client/companion/HttpConnectionPool/HttpConnectionPool.h"
#include <boost/beast/http.hpp>
#include <map>
#include <mutex>
#include <functional>
namespace nebula::client {
namespace client_http=boost::beast::http;
using ProxyRequest=client_http::request<client_http::string_body>;
using ProxyResponse=client_http::response<client_http::string_body>;
// 只在C++进程内传递，绝不序列化给浏览器；后台任务绑定启动时的账号，避免切号串数据。
struct BoundServerSession {std::string account,cookie,csrf;std::uint64_t epoch{};};
/** 固定服务器访问器；普通请求/SSE/WS共享凭据快照，目标不来自浏览器输入。 */
class UpstreamTransport final {
public:
    explicit UpstreamTransport(ServerTrust trust,bool reuseHttp=true):trust_(std::move(trust)),http_(trust_,reuseHttp){}
    HttpPoolStatistics httpStatistics()const{return http_.statistics();}
    const ServerTrust&trust()const noexcept{return trust_;}
    std::string origin()const;
    std::string cookie()const;
    void restoreCookie(std::string cookie);
    void clearCookie();
    ProxyRequest prepare(ProxyRequest request)const;
    ProxyRequest prepareBound(ProxyRequest request,const BoundServerSession&session)const{return prepareWithCookie(std::move(request),session.cookie);}
    ProxyResponse request(ProxyRequest request);
    void acceptCookies(const client_http::fields&headers);
    std::string currentAccount()const;
    BoundServerSession session()const;
    void setCredentialSink(std::function<void(const BoundServerSession&)>sink){credentialSink_=std::move(sink);}
    ProxyResponse requestBound(const BoundServerSession&session,ProxyRequest request)const;
private:
    ProxyRequest prepareWithCookie(ProxyRequest request,const std::string&cookie)const;
    ServerTrust trust_;mutable HttpConnectionPool http_;mutable std::mutex mutex_;std::mutex authOperations_;
    std::string cookie_,account_,csrf_;std::uint64_t sessionEpoch_{};
    std::function<void(const BoundServerSession&)>credentialSink_;
};
}
