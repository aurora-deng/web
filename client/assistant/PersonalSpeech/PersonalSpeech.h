#pragma once
#include "client/companion/UpstreamTransport/UpstreamTransport.h"
#include "client/storage/SqliteStore/SqliteStore.h"
#include <boost/json.hpp>
namespace nebula::client {
/** 每账号自己的音色、DPAPI Key 与持久预算；只合成固定试听，不擅自发送私人聊天。 */
class PersonalSpeech final {
public:
    PersonalSpeech(SqliteStore&,std::string accountNamespace,UpstreamTransport&);
    ~PersonalSpeech();
    ProxyResponse handle(const std::string&route,const ProxyRequest&,const boost::json::object&,const BoundServerSession&);
private:struct Impl;std::unique_ptr<Impl>impl_;
};
}
