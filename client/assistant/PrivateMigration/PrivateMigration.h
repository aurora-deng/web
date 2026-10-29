#pragma once
#include "client/assistant/PrivateHistory/PrivateHistory.h"
#include "client/assistant/LocalKnowledge/LocalKnowledge.h"
#include "client/companion/UpstreamTransport/UpstreamTransport.h"
#include <boost/json.hpp>
namespace nebula::client {
/** 旧服务器只读导出 -> 本机校验落盘 -> 用户另行确认清除；三个步骤不可合并。 */
class PrivateMigration final{
public:
    PrivateMigration(SqliteStore&store,PrivateHistory&history,LocalKnowledge&knowledge,std::filesystem::path root,UpstreamTransport&upstream);
    boost::json::value resources(const BoundServerSession&identity);
    boost::json::value pages();
    static boost::json::value verifyForImport(const boost::json::object&archive,const std::string&account);
    boost::json::value importPage(const BoundServerSession&identity,const boost::json::object&request);
    boost::json::value purge(const BoundServerSession&identity,const std::string&receipt,bool confirmed);
private:SqliteStore&store_;PrivateHistory&history_;LocalKnowledge&knowledge_;std::filesystem::path root_;UpstreamTransport&upstream_;
};
}
