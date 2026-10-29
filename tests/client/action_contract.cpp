#include "client/assistant/ActionService/ActionService.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include <iostream>
using namespace nebula::client;
void check(bool x,const char*m){if(!x)throw std::runtime_error(m);}
int main(){try{const auto path=std::filesystem::absolute("action-contract-"+randomToken()+".sqlite3");{SqliteStore store(path);ActionService actions(store);
    check(!ActionService::parseModelCall("请直接运行 rm -rf"),"free text executed");check(!ActionService::parseModelCall(R"({"type":"tool_call","tool":"shell","arguments":{}})"),"unknown tool accepted");
    const auto proposal=actions.propose("send_message",{{"conversationId","7"},{"text","经过确认的消息"}}).as_object();int sends=0;
    auto execute=[&](const std::string&,const boost::json::object&args,const std::string&operation){++sends;check(args.at("text")=="经过确认的消息"&&operation.starts_with("agent:"),"changed confirmation payload");return boost::json::object{{"ok",true}};};
    bool rejected=false;try{actions.confirm(std::string(proposal.at("id").as_string()),"changed",execute);}catch(...){rejected=true;}check(rejected&&sends==0,"digest bypassed");
    const auto id=std::string(proposal.at("id").as_string()),digest=std::string(proposal.at("digest").as_string());actions.confirm(id,digest,execute);actions.confirm(id,digest,execute);check(sends==1,"repeated confirmation repeated send");
    auto cancelled=actions.propose("update_profile",{{"displayName","甲"},{"biography","乙"}}).as_object();actions.cancel(std::string(cancelled.at("id").as_string()));rejected=false;try{actions.confirm(std::string(cancelled.at("id").as_string()),std::string(cancelled.at("digest").as_string()),execute);}catch(...){rejected=true;}check(rejected&&sends==1,"cancelled proposal executed");}
    std::filesystem::remove(path);std::cout<<"PASS structured tool allowlist, payload-bound confirmation, duplicate confirmation and cancellation\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
