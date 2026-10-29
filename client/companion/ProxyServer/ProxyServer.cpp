#include "ProxyServer.h"
#include "client/companion/LoopbackPolicy/LoopbackPolicy.h"
#include "client/companion/BoundedPool/BoundedPool.h"
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <atomic>
#include <fstream>
#include <map>
#include <optional>
namespace nebula::client {
namespace net=boost::asio;namespace beast=boost::beast;namespace http=beast::http;using tcp=net::ip::tcp;
ProxyResponse localJson(int status,boost::json::value value){ProxyResponse response{static_cast<http::status>(status),11};response.set(http::field::content_type,"application/json; charset=utf-8");response.set(http::field::cache_control,"no-store");response.set("X-Content-Type-Options","nosniff");response.body()=boost::json::serialize(value);response.keep_alive(false);response.prepare_payload();return response;}
namespace {
struct Ticket{std::function<void()>release;~Ticket(){if(release)release();}};
struct Closeable{virtual~Closeable()=default;virtual void close()=0;virtual bool stale(std::uint64_t)const{return false;}};
std::string pathOf(const ProxyRequest&request){std::string result(request.target());result.resize(result.find('?')==std::string::npos?result.size():result.find('?'));return result;}
bool writing(const ProxyRequest&r){return r.method()!=http::verb::get&&r.method()!=http::verb::head;}
class HttpClient;
void startStream(const std::shared_ptr<ProxyServer::State>&state,beast::tcp_stream socket,ProxyRequest request,std::shared_ptr<Ticket>connection);
void startWebSocket(const std::shared_ptr<ProxyServer::State>&state,beast::tcp_stream socket,ProxyRequest request,std::shared_ptr<Ticket>connection);
}
struct ProxyServer::State:std::enable_shared_from_this<State>{
    net::io_context io;tcp::acceptor listener,listener6;ProxyServerConfig config;UpstreamTransport&upstream;LocalAssistantHandler local;LoopbackPolicy policy;
    BoundedPool work{4,24},reads{8,8},writes{2,16};
    std::atomic<std::size_t>connections{},streams{},bodyBytes{};std::atomic_bool stopping{};
    std::vector<std::weak_ptr<Closeable>>open;
    std::map<std::string,std::pair<std::filesystem::path,std::string>>assets;
    State(ProxyServerConfig c,UpstreamTransport&u,LocalAssistantHandler l):listener(io),listener6(io),config(std::move(c)),upstream(u),local(std::move(l)),policy(config.port){
        if(!config.port||!config.connections||config.connections>32||!config.streams||config.streams>8)throw std::runtime_error("Invalid loopback limits");
        const auto root=std::filesystem::canonical(config.frontend);
        for(const auto&entry:std::filesystem::recursive_directory_iterator(root)){
            if(entry.is_symlink()||!entry.is_regular_file())continue;const auto extension=entry.path().extension().string();
            std::string mime=extension==".js"?"text/javascript; charset=utf-8":extension==".css"?"text/css; charset=utf-8":extension==".html"?"text/html; charset=utf-8":extension==".svg"?"image/svg+xml":extension==".png"?"image/png":"";
            if(mime.empty())continue;const auto relative=entry.path().lexically_relative(root).generic_string();
            assets.emplace("/phase11/"+relative,std::make_pair(entry.path(),mime));
            if(entry.path().filename()=="index.html")assets.emplace("/phase11/"+relative.substr(0,relative.size()-10),std::make_pair(entry.path(),mime));
        }
        listener.open(tcp::v4());listener.set_option(net::socket_base::reuse_address(false));listener.bind({net::ip::make_address_v4("127.0.0.1"),config.port});listener.listen(32);
        // localhost 在 Windows 常先解析到 ::1。只监听 IPv4 会让每次新连接先等 IPv6 失败，
        // 实测约多2秒；两个 acceptor 共用连接/流/缓冲预算，均只绑定回环地址。
        listener6.open(tcp::v6());listener6.set_option(net::ip::v6_only(true));listener6.set_option(net::socket_base::reuse_address(false));listener6.bind({net::ip::make_address_v6("::1"),config.port});listener6.listen(32);
    }
    std::shared_ptr<Ticket>permit(std::atomic<std::size_t>&counter,std::size_t amount,std::size_t limit){
        auto old=counter.fetch_add(amount);if(old+amount>limit){counter.fetch_sub(amount);return{};}
        const std::weak_ptr<State>weak=shared_from_this();auto*address=&counter;auto ticket=std::make_shared<Ticket>();
        ticket->release=[weak,address,amount]{if(auto state=weak.lock())address->fetch_sub(amount);};return ticket;
    }
    void track(const std::shared_ptr<Closeable>&connection){std::erase_if(open,[](const auto&item){return item.expired();});open.push_back(connection);}
    void identityChanged(std::uint64_t previous){const auto current=upstream.session().epoch;if(current==previous)return;for(auto&weak:open)if(auto item=weak.lock();item&&item->stale(current))item->close();}
    void accept(tcp::acceptor&source);
    void stop(){if(stopping.exchange(true))return;const std::weak_ptr<State>weak=shared_from_this();net::post(io,[weak]{if(auto s=weak.lock()){boost::system::error_code ignored;s->listener.close(ignored);s->listener6.close(ignored);for(auto&weak:s->open)if(auto item=weak.lock())item->close();}});}
    void shutdown(){work.shutdown();reads.shutdown();writes.shutdown();}
};
namespace {
class HttpClient:public Closeable,public std::enable_shared_from_this<HttpClient>{
    std::weak_ptr<ProxyServer::State>state_;beast::tcp_stream socket_;beast::flat_buffer buffer_{32768};
    http::request_parser<http::string_body>parser_;std::shared_ptr<Ticket>connection_,bodyPermit_;
    bool localAuthenticated_{};
public:
    HttpClient(const std::shared_ptr<ProxyServer::State>&state,tcp::socket socket,std::shared_ptr<Ticket>permit):state_(state),socket_(std::move(socket)),connection_(std::move(permit)){parser_.header_limit(16384);parser_.body_limit(10*1024*1024);}
    void close()override{boost::system::error_code ignored;socket_.socket().shutdown(tcp::socket::shutdown_both,ignored);socket_.socket().close(ignored);}
    void start(){socket_.expires_after(std::chrono::seconds(10));auto self=shared_from_this();http::async_read_header(socket_,buffer_,parser_,[self](boost::system::error_code error,std::size_t){if(error)return;try{self->header();}catch(...){self->reply(localJson(400,{{"message","无效的本机请求"}}));}});}
    void reply(ProxyResponse response){
        response.keep_alive(false);response.set("X-Content-Type-Options","nosniff");response.set("Referrer-Policy","same-origin");
        for(const auto name:{"Access-Control-Allow-Origin","Access-Control-Allow-Credentials","Access-Control-Allow-Headers","Access-Control-Allow-Methods"})response.erase(name);
        if(auto state=state_.lock();state&&localAuthenticated_){const auto session=state->upstream.session();response.set("X-Nebula-Session-Version",std::to_string(session.epoch));if(!session.account.empty())response.set("X-Nebula-Account",session.account);}
        response.prepare_payload();auto message=std::make_shared<ProxyResponse>(std::move(response));auto self=shared_from_this();socket_.expires_after(std::chrono::seconds(10));
        http::async_write(socket_,*message,[self,message](boost::system::error_code,std::size_t){self->close();});
    }
    void header(){
        auto s=state_.lock();if(!s||s->stopping)return close();const auto&r=parser_.get();const auto path=pathOf(r);
        if(!s->policy.validHost(r)||!s->policy.validOrigin(r,writing(r)||beast::websocket::is_upgrade(r)))return reply(localJson(403,{{"message","本机入口拒绝了未知Host或Origin"}}));
        localAuthenticated_=s->policy.authenticated(r);
        const bool api=path.starts_with("/api/")||path=="/events"||path=="/ws"||path=="/metrics"||path=="/fast"||path.starts_with("/health/");
        if(api&&path!="/api/local/session/info"&&path!="/api/local/session/bootstrap"&&!s->policy.authenticated(r))return reply(localJson(401,{{"message","请从本机入口重新打开网页"}}));
        if(path.starts_with("/api/local/assistant/")&&writing(r)&&!s->policy.csrf(r))return reply(localJson(403,{{"message","本机CSRF校验失败"}}));
        const auto length=parser_.content_length().value_or(parser_.chunked()?10*1024*1024:0);
        if(length){bodyPermit_=s->permit(s->bodyBytes,static_cast<std::size_t>(length),32*1024*1024);if(!bodyPermit_)return reply(localJson(503,{{"message","本机请求缓冲已满，请稍后重试"}}));}
        if(parser_.is_done())return dispatch();
        auto self=shared_from_this();http::async_read(socket_,buffer_,parser_,[self](boost::system::error_code error,std::size_t){if(error)return;try{self->dispatch();}catch(...){self->reply(localJson(400,{{"message","本机请求处理失败"}}));}});
    }
    void dispatch(){
        auto s=state_.lock();if(!s||s->stopping)return close();auto request=parser_.release();const auto path=pathOf(request);
        if(path=="/api/local/session/bootstrap"){
            if(request.method()!=http::verb::post||request.body().size()>4096)return reply(localJson(400,{{"message","无效的入口引导"}}));
            const auto json=boost::json::parse(request.body());const auto*token=json.is_object()?json.as_object().if_contains("token"):nullptr;
            if(!token||!token->is_string())return reply(localJson(400,{{"message","缺少入口凭据"}}));
            const auto cookie=s->policy.bootstrap(std::string(token->as_string()));if(cookie.empty())return reply(localJson(403,{{"message","入口凭据已使用或无效"}}));
            auto response=localJson(200,{{"ok",true}});response.set(http::field::set_cookie,cookie);return reply(std::move(response));
        }
        if(path=="/api/local/session/info"){
            const bool authenticated=s->policy.authenticated(request);const auto session=s->upstream.session();
            boost::json::object info{{"available",true},{"authenticated",authenticated}};
            if(authenticated){info["csrfToken"]=s->policy.csrfToken();info["accountId"]=session.account;info["sessionVersion"]=session.epoch;
                const auto pool=s->upstream.httpStatistics();info["httpPool"]=boost::json::object{{"connected",pool.connected},{"reused",pool.reused},{"discarded",pool.discarded},{"readRetries",pool.readRetries},{"failures",pool.failures},{"active",pool.active},{"idle",pool.idle}};}
            return reply(localJson(200,std::move(info)));
        }
        if(path=="/"){ProxyResponse redirect{http::status::see_other,11};redirect.set(http::field::location,"/phase11/");return reply(std::move(redirect));}
        if(const auto found=s->assets.find(path);found!=s->assets.end()){
            if(request.method()!=http::verb::get&&request.method()!=http::verb::head)return reply(localJson(405,{{"message","仅支持读取页面"}}));
            const auto size=std::filesystem::file_size(found->second.first);if(size>4*1024*1024)return reply(localJson(413,{{"message","静态资源超过限制"}}));
            std::ifstream input(found->second.first,std::ios::binary);if(!input)return reply(localJson(404,{{"message","页面资源不存在"}}));
            ProxyResponse response{http::status::ok,11};response.set(http::field::content_type,found->second.second);response.set(http::field::cache_control,"no-cache");
            if(request.method()!=http::verb::head)response.body()=std::string(std::istreambuf_iterator<char>(input),{});
            return reply(std::move(response));
        }
        if(path.starts_with("/api/local/")){
            // 这个分支无论成功、未知接口或异常，都不会退回 Linux 代理。
            if(!path.starts_with("/api/local/assistant/"))return reply(localJson(404,{{"message","未知本机接口"}}));
            const auto session=s->upstream.session();const auto claimed=request["X-Nebula-Account"];
            if(session.account.empty())return reply(localJson(401,{{"message","请先登录，再开启本机私人空间"}}));
            if(path!="/api/local/assistant/capabilities"&&claimed!=session.account)return reply(localJson(409,{{"message","账号已经改变，请刷新私人助手"}}));
            auto self=shared_from_this();auto bodyPermit=bodyPermit_;
            if(!s->work.submit([weak=state_,self,request=std::move(request),session,bodyPermit]{if(auto state=weak.lock()){
                ProxyResponse response;try{response=state->local(request,session);}catch(...){response=localJson(500,{{"message","本机私人操作失败"}});}
                net::post(state->io,[weak,self,response=std::move(response)]()mutable{if(auto state=weak.lock();state&&!state->stopping)self->reply(std::move(response));});
            }}))reply(localJson(503,{{"message","本机任务队列已满"}}));return;
        }
        if(path=="/ws"&&beast::websocket::is_upgrade(request))return startWebSocket(s,std::move(socket_),std::move(request),connection_);
        const bool download=path.starts_with("/api/files/")||path.find("/media/")!=std::string::npos||path.starts_with("/api/announcements/")||path.starts_with("/api/admin/speech/audio/")||path.starts_with("/api/me/speech/audio/");
        if(path=="/events"||(download&&request.method()==http::verb::get))return startStream(s,std::move(socket_),std::move(request),connection_);
        if(!(path.starts_with("/api/")||path.starts_with("/health/")||path=="/metrics"||path=="/fast"))return reply(localJson(404,{{"message","没有这个页面或接口"}}));
        auto self=shared_from_this();auto bodyPermit=bodyPermit_;
        if(!s->work.submit([weak=state_,self,request=std::move(request),bodyPermit]{if(auto state=weak.lock()){
            const auto previous=state->upstream.session().epoch;ProxyResponse response;
            try{response=state->upstream.request(std::move(request));}catch(...){response=localJson(502,{{"message","无法连接服务器，或HTTPS证书验证未通过"}});}
            net::post(state->io,[weak,self,previous,response=std::move(response)]()mutable{if(auto state=weak.lock();state&&!state->stopping){state->identityChanged(previous);self->reply(std::move(response));}});
        }}))reply(localJson(503,{{"message","本机代理繁忙，请稍后重试"}}));
    }
};
#include "ProxyStreams.ipp"
}
void ProxyServer::State::accept(tcp::acceptor&source){
    const std::weak_ptr<State>weak=shared_from_this();auto*acceptor=&source;source.async_accept([weak,acceptor](boost::system::error_code error,tcp::socket socket){if(auto state=weak.lock()){
        if(!error&&!state->stopping){auto permit=state->permit(state->connections,1,state->config.connections);if(permit){auto client=std::make_shared<HttpClient>(state,std::move(socket),std::move(permit));state->track(client);client->start();}}
        if(!state->stopping)state->accept(*acceptor);
    }});
}
ProxyServer::ProxyServer(ProxyServerConfig config,UpstreamTransport&upstream,LocalAssistantHandler local):state_(std::make_shared<State>(std::move(config),upstream,std::move(local))){state_->accept(state_->listener);state_->accept(state_->listener6);}
ProxyServer::~ProxyServer(){stop();if(state_){state_->io.poll();state_->shutdown();}}
void ProxyServer::run(){state_->io.run();state_->shutdown();}
void ProxyServer::stop(){if(state_)state_->stop();}
std::string ProxyServer::launchUrl()const{return state_->policy.origin()+"/phase11/#companion="+state_->policy.launchToken();}
}
