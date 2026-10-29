#include "PrivateHistory.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include <stdexcept>
namespace nebula::client {
namespace {void validMemory(const std::string&text){if(text.empty()||text.size()>2048)throw std::runtime_error("记忆应为非空短文本，最多2 KiB");}}
PrivateHistory::PrivateHistory(SqliteStore&store):store_(store){
    store_.execute("CREATE TABLE IF NOT EXISTS private_threads(id TEXT PRIMARY KEY,surface TEXT NOT NULL CHECK(surface IN ('friend','sidebar')),title TEXT NOT NULL,created INTEGER NOT NULL DEFAULT(unixepoch()))");
    store_.execute("CREATE TABLE IF NOT EXISTS private_messages(id INTEGER PRIMARY KEY AUTOINCREMENT,thread_id TEXT NOT NULL REFERENCES private_threads(id)ON DELETE CASCADE,request_id TEXT NOT NULL,role TEXT NOT NULL CHECK(role IN ('user','assistant')),body TEXT NOT NULL,state TEXT NOT NULL,UNIQUE(thread_id,request_id,role))");
    store_.execute("CREATE INDEX IF NOT EXISTS private_messages_thread ON private_messages(thread_id,id)");
    store_.execute("CREATE TABLE IF NOT EXISTS private_memories(id TEXT PRIMARY KEY,body TEXT NOT NULL,enabled INTEGER NOT NULL DEFAULT 1,created INTEGER NOT NULL DEFAULT(unixepoch()))");
}
PrivateThread PrivateHistory::createThread(std::string surface,std::string title){
    if((surface!="friend"&&surface!="sidebar")||title.empty()||title.size()>200)throw std::runtime_error("无效助手入口或标题");
    PrivateThread result{randomToken(),std::move(surface),std::move(title)};
    store_.transact([&]{if(std::stoul(*store_.query("SELECT count(*) FROM private_threads")[0][0])>=1000)throw std::runtime_error("助手会话达到1000条上限，请先整理");store_.execute("INSERT INTO private_threads(id,surface,title)VALUES(?,?,?)",{result.id,result.surface,result.title});});return result;
}
std::vector<PrivateThread>PrivateHistory::threads(const std::string&surface){
    if(surface!="friend"&&surface!="sidebar")throw std::runtime_error("无效助手入口");
    std::vector<PrivateThread>result;for(const auto&r:store_.query("SELECT id,surface,title FROM private_threads WHERE surface=? ORDER BY created DESC,id DESC LIMIT 100",{surface}))result.push_back({*r[0],*r[1],*r[2]});return result;
}
std::vector<PrivateMessage>PrivateHistory::messages(const std::string&thread,std::uint64_t before,std::size_t limit){
    if(!limit||limit>100)throw std::runtime_error("历史页大小应为1..100");
    std::vector<PrivateMessage>result;const auto rows=store_.query("SELECT id,request_id,role,body,state FROM(SELECT id,request_id,role,body,state FROM private_messages WHERE thread_id=? AND (CAST(? AS INTEGER)=0 OR id<?) ORDER BY id DESC LIMIT ?)ORDER BY id",{thread,std::to_string(before),std::to_string(before),std::to_string(limit)});
    for(const auto&r:rows)result.push_back({*r[0],*r[1],*r[2],*r[3],*r[4]});return result;
}
PrivateMessage PrivateHistory::append(const std::string&thread,std::string requestId,std::string role,std::string body,std::string state){
    if(requestId.empty()||requestId.size()>128||body.size()>65536||(role!="user"&&role!="assistant")||
        (state!="completed"&&state!="streaming"&&state!="cancelled"&&state!="failed"))throw std::runtime_error("助手消息超出限制");
    PrivateMessage result;
    store_.transact([&]{
        const auto previous=store_.query("SELECT id,request_id,role,body,state FROM private_messages WHERE thread_id=? AND request_id=? AND role=?",{thread,requestId,role});
        if(!previous.empty()){const auto&r=previous.front();if(*r[3]!=body||*r[4]!=state)throw std::runtime_error("相同助手操作编号对应了不同消息");result={*r[0],*r[1],*r[2],*r[3],*r[4]};return;}
        store_.execute("INSERT INTO private_messages(thread_id,request_id,role,body,state)VALUES(?,?,?,?,?)",{thread,requestId,role,body,state});
        result={*store_.query("SELECT last_insert_rowid()")[0][0],requestId,role,body,state};
    });return result;
}
void PrivateHistory::finish(const std::string&thread,const std::string&requestId,std::string body,std::string state){
    if(body.size()>65536||(state!="completed"&&state!="cancelled"&&state!="failed"))throw std::runtime_error("无效助手终态");
    // 只允许从流式状态结束；取消后的迟到完成不能重新覆盖用户看到的结果。
    store_.execute("UPDATE private_messages SET body=?,state=? WHERE thread_id=? AND request_id=? AND role='assistant' AND state='streaming'",{body,state,thread,requestId});
}
void PrivateHistory::removeThread(const std::string&thread){store_.execute("DELETE FROM private_threads WHERE id=?",{thread});}
PrivateMemory PrivateHistory::remember(std::string text,bool confirmed){
    validMemory(text);if(!confirmed)throw std::runtime_error("请先确认这条长期记忆，或明确发出记住指令");PrivateMemory result{randomToken(),std::move(text),true};
    store_.transact([&]{if(std::stoul(*store_.query("SELECT count(*) FROM private_memories")[0][0])>=200)throw std::runtime_error("长期记忆达到200条上限");store_.execute("INSERT INTO private_memories(id,body)VALUES(?,?)",{result.id,result.text});});return result;
}
std::vector<PrivateMemory>PrivateHistory::memories(){std::vector<PrivateMemory>result;for(const auto&r:store_.query("SELECT id,body,enabled FROM private_memories ORDER BY created DESC,id DESC LIMIT 200"))result.push_back({*r[0],*r[1],*r[2]=="1"});return result;}
void PrivateHistory::updateMemory(const std::string&id,std::string text,bool enabled){validMemory(text);store_.execute("UPDATE private_memories SET body=?,enabled=? WHERE id=?",{text,enabled?"1":"0",id});}
void PrivateHistory::removeMemory(const std::string&id){store_.execute("DELETE FROM private_memories WHERE id=?",{id});}
}
