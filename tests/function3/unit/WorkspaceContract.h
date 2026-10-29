#pragma once
#include "backend/application/modules/chat/application/ChatWorkspaceService/ChatWorkspaceService.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include "backend/application/observability/BusinessMetrics/BusinessMetrics.h"
#include <stdexcept>

namespace webserver::chat::test {
inline void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(std::string("workspace contract: ") + message);
}
template<class F> void rejects(F action, ErrorCode expected) {
    try { action(); } catch (const ApplicationError& e) { require(e.code()==expected,"wrong rejection"); return; }
    throw std::runtime_error("workspace contract: access unexpectedly allowed");
}
/** Same use cases run against both drivers; transaction and membership rules cannot drift. */
template<class Store> void runWorkspaceContract(Store& store) {
    const auto now = std::chrono::system_clock::now();
    const auto a = store.create({0,"workspace-a","共同 Alice",{},"hash",false,false,now});
    const auto b = store.create({0,"workspace-b","共同 Bob",{},"hash",false,false,now});
    const auto c = store.create({0,"workspace-c","共同 Carol",{},"hash",false,false,now});
    const auto percent = store.create({0,"workspace-pct","共同%literal",{},"hash",false,false,now});
    ChatWorkspaceService app(store,store,store,store,store,store);
    auto page = app.users(a.id,"共同",0,1);
    require(page.items.size()==1 && page.nextCursor==b.id,"user cursor first page");
    require(app.users(a.id,"共同",page.nextCursor,10).items.size()==2,"user next page/self exclusion");
    require(app.users(a.id,"共同%",0,10).items.front().id==percent.id,"literal percent prefix");
    store.blockUser(a.id,c.id,now);
    require(app.users(a.id,"workspace-c",0,10).items.empty(),"blocked user exposed");
    const auto group=store.createConversation({0,ConversationKind::Group,"Workspace",a.id,1,now},
        {{0,a.id,MemberRole::Owner,0,0,now},{0,b.id,MemberRole::Member,0,0,now}});
    const auto secret=store.createConversation({0,ConversationKind::Group,"Secret",c.id,1,now},
        {{0,c.id,MemberRole::Owner,0,0,now}});
    const auto first=store.appendMessage({0,group.id,0,a.id,"w-1","hello 100% literal",{},now}).message;
    const auto second=store.appendMessage({0,group.id,0,b.id,"w-2","hello 100_ literal",{},now}).message;
    store.appendMessage({0,secret.id,0,c.id,"w-secret","hello private",{},now});
    auto search=app.search(b.id,0,"hello",0,1);
    require(search.items.size()==1 && search.items.front().id==second.id && search.nextCursor==second.id,"global stable cursor");
    require(app.search(b.id,0,"hello",search.nextCursor,10).items.front().id==first.id,"next search page");
    require(app.search(b.id,0,"100%",0,10).items.size()==1,"SQL wildcard interpreted as syntax");
    rejects([&]{app.context(c.id,group.id,first.id,10);},ErrorCode::Forbidden);
    rejects([&]{app.context(b.id,group.id,store.messagesAfter(secret.id,0,10).front().id,10);},ErrorCode::NotFound);
    require(app.context(b.id,group.id,first.id,1).items.size()==2,"bounded context");
    store.recallMessage(first.id,a.id,now);
    require(app.search(b.id,0,"100%",0,10).items.empty(),"recalled body searchable");
    require(app.context(b.id,group.id,first.id,1).target.body.empty(),"recalled context leaked body");
    auto pref=app.setPreferences(b.id,group.id,true,now);
    require(pref.muted && pref.version==1,"first mute revision");
    require(app.setPreferences(b.id,group.id,true,now).version==1,"same value not idempotent");
    require(!app.preferences(a.id,group.id).muted,"other member preference changed");
    const auto outbox=store.pendingOutbox(10000).size();
    {
        auto tx=store.beginTransaction();
        store.writePreference(b.id,group.id,false);
        store.appendOutbox({0,"conversation.preferences","user",b.id,"rollback",now,{}});
        // No commit: both state and its event must roll back together.
    }
    require(app.preferences(b.id,group.id).muted && store.pendingOutbox(10000).size()==outbox,"preference/outbox rollback split");
    require(app.setPreferences(b.id,group.id,false,now).version==2,"unmute revision");
    store.removeMember(group.id,b.id);
    require(app.search(b.id,0,"hello",0,10).items.empty(),"departed membership still searchable");
    rejects([&]{app.preferences(b.id,group.id);},ErrorCode::Forbidden);
    // A page of twenty attachments must not execute twenty extra queries.
    for(int i=0;i<20;++i){
        const auto message=store.appendMessage({0,group.id,0,a.id,"batch-"+std::to_string(i),"attachment batch",{},now}).message;
        const auto file=store.createFile({0,a.id,"batch.txt","text/plain",1,1,"",FileState::Pending,now,{}});
        require(store.completeFile(file.id,a.id,std::string(64,'a'),now),"complete fixture failed");
        require(store.attachFile(message.id,file.id),"attach fixture failed");
    }
    const auto beforeQueries=business_metrics::counters[business_metrics::Database].count.load();
    const auto batch=store.messagesBefore(group.id,0,20);
    const auto queryCount=business_metrics::counters[business_metrics::Database].count.load()-beforeQueries;
    require(batch.size()==20,"batch history size");
    for(const auto&message:batch)require(message.attachments.size()==1,"attachment batch mapping");
    if constexpr(requires{store.poolSnapshot();})require(queryCount==2,"history N+1 regression (expected two driver queries)");
}
}
