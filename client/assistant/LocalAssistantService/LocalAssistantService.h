#pragma once
#include "client/companion/ProxyServer/ProxyServer.h"
#include "client/assistant/DocumentImport/DocumentImport.h"
#include <filesystem>
namespace nebula::client {
/** 本机私人业务装配：每站点/账号一个库，生成任务绑定启动时的服务器身份。 */
class LocalAssistantService final{
public:
    LocalAssistantService(std::filesystem::path root,UpstreamTransport&upstream,DocumentImport importer);
    ~LocalAssistantService();
    ProxyResponse handle(const ProxyRequest&request,const BoundServerSession&identity);
    void stop();
private:struct Impl;std::unique_ptr<Impl>impl_;
};
}
