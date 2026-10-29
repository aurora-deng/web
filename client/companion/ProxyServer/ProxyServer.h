#pragma once
#include "client/companion/UpstreamTransport/UpstreamTransport.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <boost/json/value.hpp>
namespace nebula::client {
struct ProxyServerConfig {
    unsigned short port{18091};std::filesystem::path frontend;
    std::size_t connections{16},streams{8};
};
using LocalAssistantHandler=std::function<ProxyResponse(const ProxyRequest&,const BoundServerSession&)>;
/** 本机HTTP/WS/SSE入口，只负责安全检查、静态页面和有界通信；私人业务通过回调装配。 */
class ProxyServer final {
public:
    struct State;
    ProxyServer(ProxyServerConfig config,UpstreamTransport&upstream,LocalAssistantHandler local);
    ~ProxyServer();
    void run();void stop();
    std::string launchUrl()const;
private:std::shared_ptr<State>state_;
};
ProxyResponse localJson(int status,boost::json::value value);
}
