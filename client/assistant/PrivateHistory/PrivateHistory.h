#pragma once
#include "client/storage/SqliteStore/SqliteStore.h"
namespace nebula::client {
struct PrivateThread{std::string id,surface,title;};
struct PrivateMessage{std::string id,requestId,role,body,state;};
struct PrivateMemory{std::string id,text;bool enabled{};};
/** 好友入口/侧栏的线程互相隔离，记忆由同一账号库共享。原文从不走普通业务代理。 */
class PrivateHistory final {
public:
    explicit PrivateHistory(SqliteStore&store);
    PrivateThread createThread(std::string surface,std::string title);
    std::vector<PrivateThread>threads(const std::string&surface);
    std::vector<PrivateMessage>messages(const std::string&thread,std::uint64_t before=0,std::size_t limit=100);
    PrivateMessage append(const std::string&thread,std::string requestId,std::string role,std::string body,std::string state);
    void finish(const std::string&thread,const std::string&requestId,std::string body,std::string state);
    void removeThread(const std::string&thread);
    PrivateMemory remember(std::string text,bool confirmed);
    std::vector<PrivateMemory>memories();
    void updateMemory(const std::string&id,std::string text,bool enabled);
    void removeMemory(const std::string&id);
private:SqliteStore&store_;
};
}
