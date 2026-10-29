#include "client/assistant/LocalKnowledge/LocalKnowledge.h"
#include "client/assistant/PrivateHistory/PrivateHistory.h"
#include "client/storage/WindowsProtection/WindowsProtection.h"
#include <iostream>
using namespace nebula::client;
void check(bool v,const char*m){if(!v)throw std::runtime_error(m);}
int main(){try{
    const auto root=std::filesystem::current_path()/"private-data-contract";createPrivateDirectory(root);
    const auto file=root/(randomToken()+".db");std::string friendId,docId;
    {SqliteStore db(file);LocalKnowledge knowledge(db);PrivateHistory history(db);
    const auto friendThread=history.createThread("friend","私人助手"),sidebar=history.createThread("sidebar","侧栏助手");friendId=friendThread.id;
    history.append(friendThread.id,"op-1","user","只属于好友入口的历史","completed");
    check(history.messages(sidebar.id).empty(),"surfaces share history unexpectedly");check(history.threads("friend").size()==1,"friend surface listing");
    history.append(friendThread.id,"op-1","user","只属于好友入口的历史","completed");check(history.messages(friendThread.id).size()==1,"message dedup");
    history.append(friendThread.id,"op-2","assistant","","streaming");history.finish(friendThread.id,"op-2","部分回答","cancelled");history.finish(friendThread.id,"op-2","迟到回答","completed");check(history.messages(friendThread.id).back().body=="部分回答","cancelled terminal overwritten");
    bool rejected=false;try{history.remember("未经确认的记忆",false);}catch(...){rejected=true;}check(rejected,"memory saved without confirmation");
    const auto memory=history.remember("用户喜欢用C++学习系统架构",true);check(history.memories().size()==1,"shared memory missing");history.updateMemory(memory.id,"偏好简洁类比",false);check(!history.memories()[0].enabled,"memory disable");
    const auto document=knowledge.importText("架构笔记","notes.md","Reactor负责网络事件通知，业务层通过接口处理聊天。",false);docId=document.id;
    check(knowledge.search("网络事件").empty(),"unauthorized document leaked to retrieval");knowledge.configure(docId,true,true);check(!knowledge.search("网络事件").empty(),"Chinese lexical retrieval failed");
    check(knowledge.importText("duplicate","copy.md","Reactor负责网络事件通知，业务层通过接口处理聊天。",true).id==docId,"document checksum dedup");
    knowledge.configure(docId,false,true);check(knowledge.search("Reactor").empty(),"disabled document retrieved");knowledge.configure(docId,true,true);
    check(LocalKnowledge::terms("hello 世界").contains("世界"),"CJK bigram missing");}
    {SqliteStore db(file);LocalKnowledge knowledge(db);PrivateHistory history(db);check(history.messages(friendId).size()==2,"local history lost after reopen");check(knowledge.documents().size()==1,"local documents lost after reopen");knowledge.remove(docId);check(knowledge.search("网络").empty(),"deleted chunks/terms remain");}
    std::cout<<"PASS separate private histories, terminal cancellation, confirmed memory, authorized CJK lexical retrieval and restart persistence\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
