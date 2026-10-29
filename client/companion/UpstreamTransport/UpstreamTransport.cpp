#include "UpstreamTransport.h"
#include <boost/json.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <stdexcept>
namespace nebula::client {
namespace http=boost::beast::http;
std::string UpstreamTransport::origin()const{return "https://"+trust_.host+":"+std::to_string(trust_.port);}
std::string UpstreamTransport::cookie()const{std::lock_guard lock(mutex_);return cookie_;}
std::string UpstreamTransport::currentAccount()const{std::lock_guard lock(mutex_);return account_;}
BoundServerSession UpstreamTransport::session()const{std::lock_guard lock(mutex_);return{account_,cookie_,csrf_,sessionEpoch_};}
void UpstreamTransport::restoreCookie(std::string value){
    if(!value.starts_with("phase11_session=")||value.size()>1024||value.find_first_of("\r\n;")!=std::string::npos)throw std::runtime_error("Invalid stored server session");
    std::lock_guard lock(mutex_);cookie_=std::move(value);account_.clear();csrf_.clear();http_.invalidate(++sessionEpoch_);
}
void UpstreamTransport::clearCookie(){std::lock_guard lock(mutex_);cookie_.clear();account_.clear();csrf_.clear();http_.invalidate(++sessionEpoch_);}
ProxyRequest UpstreamTransport::prepare(ProxyRequest request)const {
    return prepareWithCookie(std::move(request),cookie());
}
ProxyRequest UpstreamTransport::prepareWithCookie(ProxyRequest request,const std::string&session)const {
    const std::string target(request.target());
    if(target.empty()||target[0]!='/'||target.starts_with("//")||target.find_first_of("\r\n\\")!=std::string::npos||target.size()>4096)
        throw std::runtime_error("Proxy target must be a relative business path");
    // 不转发浏览器的本地凭据、代理头或 hop-by-hop 头；不存在任意URL转发入口。
    for(const auto name:{http::field::cookie,http::field::host,http::field::connection,http::field::proxy_authorization,
        http::field::proxy_connection,http::field::forwarded,http::field::via,http::field::transfer_encoding,
        http::field::upgrade,http::field::expect})request.erase(name);
    request.erase("X-Forwarded-Host");request.erase("X-Forwarded-Proto");request.erase("X-Forwarded-For");
    request.set(http::field::host,trust_.host+":"+std::to_string(trust_.port));
    request.set(http::field::origin,origin());request.set(http::field::user_agent,"NebulaCompanion/Phase38");
    if(!session.empty())request.set(http::field::cookie,session);
    request.set(http::field::accept_encoding,"identity");
    request.keep_alive(false);request.prepare_payload();return request;
}
void UpstreamTransport::acceptCookies(const http::fields&headers){
    for(const auto&field:headers){if(field.name()!=http::field::set_cookie)continue;std::string value(field.value());
        if(!value.starts_with("phase11_session="))continue;
        value.resize(value.find(';')==std::string::npos?value.size():value.find(';'));
        if(value.size()>1024||value.find_first_of("\r\n")!=std::string::npos)throw std::runtime_error("Invalid server session cookie");
        std::lock_guard lock(mutex_);cookie_=value=="phase11_session="?"":std::move(value);account_.clear();csrf_.clear();http_.invalidate(++sessionEpoch_);
    }
}
ProxyResponse UpstreamTransport::request(ProxyRequest request){
    const std::string target(request.target());
    const bool readsIdentity=(target=="/api/me"&&request.method()==http::verb::get)||target=="/api/auth/login";
    // 登录/退出/身份恢复串行，避免迟到 /me 清掉新登录或切到别人的私人目录。
    std::unique_lock<std::mutex>authLock(authOperations_,std::defer_lock);
    if(target.starts_with("/api/auth/")||target=="/api/me")authLock.lock();
    std::string session;std::uint64_t epoch;{std::lock_guard lock(mutex_);session=cookie_;epoch=sessionEpoch_;}
    auto outbound=prepareWithCookie(std::move(request),session);
    auto response=http_.request(std::move(outbound),epoch);acceptCookies(response.base());
    if(readsIdentity&&response.result_int()==200){boost::system::error_code error;const auto body=boost::json::parse(response.body(),error);
        if(!error&&body.is_object())if(const auto*user=body.as_object().if_contains("user");user&&user->is_object())if(const auto*id=user->as_object().if_contains("id")){
            std::string account;if(id->is_string())account=std::string(id->as_string());else if(id->is_uint64())account=std::to_string(id->as_uint64());else if(id->is_int64()&&id->as_int64()>0)account=std::to_string(id->as_int64());
            if(account.empty()||account.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Server identity is malformed");
            const auto*csrf=body.as_object().if_contains("csrfToken");
            if(!csrf||!csrf->is_string())throw std::runtime_error("Server CSRF token missing");
            std::lock_guard lock(mutex_);if(sessionEpoch_==epoch||target=="/api/auth/login"){account_=std::move(account);csrf_=std::string(csrf->as_string());}
        }
    }
    if(target=="/api/auth/logout")clearCookie();
    else if(response.result_int()==401&&!target.starts_with("/api/auth/")){
        std::lock_guard lock(mutex_);if(sessionEpoch_==epoch){cookie_.clear();account_.clear();csrf_.clear();http_.invalidate(++sessionEpoch_);}
    }
    if(credentialSink_&&(target.starts_with("/api/auth/")||target=="/api/me"))credentialSink_(this->session());
    response.erase(http::field::set_cookie);response.erase(http::field::connection);response.erase(http::field::transfer_encoding);
    response.keep_alive(false);response.prepare_payload();return response;
}
ProxyResponse UpstreamTransport::requestBound(const BoundServerSession&session,ProxyRequest request)const {
    if(session.account.empty()||session.cookie.empty())throw std::runtime_error("Verified server account required");
    request.set("X-CSRF-Token",session.csrf);auto outbound=prepareWithCookie(std::move(request),session.cookie);
    auto response=http_.request(std::move(outbound),session.epoch);response.erase(http::field::set_cookie);return response;
}
}
