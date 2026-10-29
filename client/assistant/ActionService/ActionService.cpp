#include "ActionService.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include <chrono>
#include <map>
#include <stdexcept>
namespace nebula::client {
namespace j=boost::json;
namespace{
std::int64_t now(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::string canonical(const std::string&tool,const j::object&arguments){
    std::map<std::string,std::string>allowed;if(tool=="send_message")allowed={{"conversationId",""},{"text",""}};
    else if(tool=="publish_activity")allowed={{"body",""},{"categoryId",""},{"destination",""},{"visibility",""},{"confirmPublic",""}};
    else if(tool=="update_profile")allowed={{"displayName",""},{"biography",""}};
    else throw std::runtime_error("工具不在写操作白名单中");
    if(arguments.size()!=allowed.size())throw std::runtime_error("工具参数数量不匹配");
    for(auto&[key,value]:allowed){const auto*field=arguments.if_contains(key);if(!field||!field->is_string()||field->as_string().size()>8000)throw std::runtime_error("工具字段缺失或超出限制");value=std::string(field->as_string());}
    if(tool=="send_message"&&(allowed["conversationId"].empty()||allowed["conversationId"].find_first_not_of("0123456789")!=std::string::npos||allowed["text"].empty()))throw std::runtime_error("消息需要有效会话和正文");
    if(tool=="publish_activity"&&(allowed["destination"]!="profile"&&allowed["destination"]!="tree"&&allowed["destination"]!="both"))throw std::runtime_error("无效动态渠道");
    if(tool=="publish_activity"&&allowed["destination"]!="profile"&&allowed["confirmPublic"]!="true")throw std::runtime_error("树频道向所有登录用户开放，需要明确公开确认");
    j::object sorted;for(const auto&[key,value]:allowed)sorted[key]=value;return j::serialize(sorted);
}
j::value row(const Row&r){return j::object{{"id",*r[0]},{"tool",*r[1]},{"arguments",j::parse(*r[2])},{"digest",*r[3]},{"state",*r[4]},{"expires",std::stoll(*r[5])},{"result",j::parse(*r[6])}};}
}
ActionService::ActionService(SqliteStore&store):store_(store){store_.execute("CREATE TABLE IF NOT EXISTS action_proposals(id TEXT PRIMARY KEY,tool TEXT NOT NULL,payload TEXT NOT NULL,digest TEXT NOT NULL,state TEXT NOT NULL,expires INTEGER NOT NULL,result TEXT NOT NULL DEFAULT '{}')");store_.execute("UPDATE action_proposals SET state='retryable' WHERE state='executing'");}
j::value ActionService::capabilities(){return j::object{{"schemaVersion",1},{"modelAutomaticExecution",false},{"readTools",j::array{"search_authorized_documents","query_my_profile","query_visible_conversations","open_page"}},{"confirmedWriteTools",j::array{"send_message","publish_activity","update_profile"}},{"fallback","answer-and-drafts"}};}
std::optional<std::pair<std::string,j::object>>ActionService::parseModelCall(const std::string&output){
    if(output.size()>12000)return{};boost::system::error_code error;auto value=j::parse(output,error);if(error||!value.is_object()||value.as_object().size()!=3)return{};
    const auto&o=value.as_object();const auto*type=o.if_contains("type"),*tool=o.if_contains("tool"),*args=o.if_contains("arguments");if(!type||!type->is_string()||type->as_string()!="tool_call"||!tool||!tool->is_string()||!args||!args->is_object())return{};
    try{canonical(std::string(tool->as_string()),args->as_object());}catch(...){return{};}return std::make_pair(std::string(tool->as_string()),args->as_object());
}
j::value ActionService::propose(const std::string&tool,const j::object&arguments){const auto payload=canonical(tool,arguments);const auto id=randomToken();const auto digest=sha256(tool+"\n"+payload);const auto expires=std::to_string(now()+600);
    store_.transact([&]{store_.execute("DELETE FROM action_proposals WHERE state IN('completed','cancelled') AND expires<unixepoch()-86400");if(std::stoul(*store_.query("SELECT count(*) FROM action_proposals")[0][0])>=256)throw std::runtime_error("确认卡达到256条上限，请整理");store_.execute("INSERT INTO action_proposals(id,tool,payload,digest,state,expires)VALUES(?,?,?,?,'pending',?)",{id,tool,payload,digest,expires});});return row(store_.query("SELECT id,tool,payload,digest,state,expires,result FROM action_proposals WHERE id=?",{id})[0]);}
j::value ActionService::list(){j::array result;for(const auto&r:store_.query("SELECT id,tool,payload,digest,state,expires,result FROM action_proposals ORDER BY rowid DESC LIMIT 100"))result.push_back(row(r));return result;}
j::value ActionService::confirm(const std::string&id,const std::string&digest,ConfirmedActionExecutor execute){std::lock_guard lock(execution_);auto rows=store_.query("SELECT id,tool,payload,digest,state,expires,result FROM action_proposals WHERE id=?",{id});if(rows.empty())throw std::runtime_error("确认卡不存在");auto r=rows[0];if(*r[3]!=digest)throw std::runtime_error("确认载荷摘要不匹配");if(*r[4]=="completed")return row(r);if(*r[4]=="cancelled")throw std::runtime_error("此操作已取消，不能继续执行");if(*r[4]=="pending"&&std::stoll(*r[5])<now())throw std::runtime_error("确认卡已过期，请重新检查目标和正文");
    store_.execute("UPDATE action_proposals SET state='executing' WHERE id=?",{id});try{const auto result=execute(*r[1],j::parse(*r[2]).as_object(),"agent:"+id);store_.execute("UPDATE action_proposals SET state='completed',result=? WHERE id=?",{j::serialize(result),id});}catch(...){store_.execute("UPDATE action_proposals SET state='retryable' WHERE id=?",{id});throw;}return row(store_.query("SELECT id,tool,payload,digest,state,expires,result FROM action_proposals WHERE id=?",{id})[0]);}
void ActionService::cancel(const std::string&id){std::lock_guard lock(execution_);store_.execute("UPDATE action_proposals SET state='cancelled' WHERE id=? AND state IN('pending','retryable')",{id});}
}
