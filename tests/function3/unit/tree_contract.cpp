#include "backend/application/modules/tree/application/TreeService/TreeService.h"
#include "backend/application/modules/tree/infrastructure/InMemoryTreeRepository/InMemoryTreeRepository.h"
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace webserver::function3;
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(Failure code, F operation) {
    try { operation(); } catch (const BusinessError &error) { check(error.code == code, "wrong error"); return; }
    throw std::runtime_error("operation unexpectedly accepted");
}
int main() {
    try {
        InMemoryTreeRepository store;
        store.seedActor({1,"作者"}); store.seedActor({2,"读者"}); store.seedActor({3,"停用",Role::User,false});
        store.seedActor({9,"管理员",Role::Moderator}); store.seedCategory({1,"生活"});
        store.seedActor({10,"站点管理员",Role::Administrator});
        store.seedMedia(1,{11,MediaKind::Image,1024,0});
        store.seedMedia(1,{12,MediaKind::Audio,1024,60000});
        store.seedMedia(2,{21,MediaKind::Image,1024,0});
        TreeService tree(store,store,store,store,store);
        PublishPost request{"post-1","今天很开心",1,"开心",{"生活","随想"},{11,12}};
        const auto first=tree.publish(1,request,1000);
        check(first.id==1 && tree.count(2)==1 && store.events().size()==1,"publish and outbox");
        check(tree.publish(1,request,2000).id==first.id && store.events().size()==1,"publish dedup");
        auto different=request; different.body="changed";
        rejects(Failure::Conflict,[&]{tree.publish(1,different,2000);});
        rejects(Failure::Unauthenticated,[&]{tree.list(0,{});});
        rejects(Failure::Forbidden,[&]{tree.list(3,{});});
        auto foreign=request; foreign.requestId="foreign"; foreign.files={21};
        rejects(Failure::Forbidden,[&]{tree.publish(1,foreign,2000);});
        auto empty=request; empty.requestId="empty"; empty.body="  "; empty.files.clear();
        rejects(Failure::Invalid,[&]{tree.publish(1,empty,2000);});
        auto malformed=empty; malformed.body=std::string("\xc0\xaf",2);
        rejects(Failure::Invalid,[&]{tree.publish(1,malformed,2000);});
        auto rollback=request; rollback.requestId="rollback"; store.failNextEvent();
        rejects(Failure::Busy,[&]{tree.publish(1,rollback,2000);});
        check(tree.count(1)==1 && store.events().size()==1,"event failure must rollback post");
        check(tree.publish(1,rollback,2001).id==2,"retry rolled back command");
        check(tree.like(1,first.id,true,3000).likes==0,"self like excluded from heat");
        check(tree.like(2,first.id,true,3000).likes==1,"like count");
        const auto rootReply=tree.reply(2,first.id,"thread-root","你好",3100);
        const auto childReply=tree.reply(1,first.id,"thread-child","谢谢你的回应",3101,rootReply.id);
        check(childReply.parent==rootReply.id&&childReply.replyToName=="读者","persistent reply target");
        check(tree.reply(1,first.id,"thread-child","谢谢你的回应",3102,rootReply.id).id==childReply.id,"thread dedup");
        rejects(Failure::Conflict,[&]{tree.reply(1,first.id,"thread-child","谢谢你的回应",3102,0);});
        rejects(Failure::Invalid,[&]{tree.reply(1,2,"foreign-thread","不能跨果实回复",3102,rootReply.id);});
        tree.removeReply(2,rootReply.id,3103);tree.removeReply(1,childReply.id,3103);
        const auto events=store.events().size();
        tree.like(2,first.id,true,3000); check(store.events().size()==events,"like idempotence");
        check(tree.like(2,first.id,false,3000).likes==0,"unlike");
        auto reply=tree.reply(2,first.id,"reply-1","祝你开心",3000);
        check(tree.reply(2,first.id,"reply-1","祝你开心",3000).id==reply.id,"reply dedup");
        tree.reply(2,first.id,"reply-2","第二条",3000);
        tree.reply(1,first.id,"reply-3","自己的回复",3000);
        check(tree.get(2,first.id).repliers==1,"unique other repliers");
        rejects(Failure::Forbidden,[&]{tree.removeReply(1,reply.id,4000);});
        tree.removeReply(2,reply.id,4000); check(tree.get(2,first.id).repliers==1,"other reply still counts");
        auto replies=tree.replies(2,first.id,0,1); check(replies.size()==1,"reply pagination");
        auto q=PostQuery{}; q.tag="生活"; q.limit=1;
        auto page=tree.list(2,q); check(page.size()==1 && page[0].id==2,"post page descending");
        q.before=page[0].id; check(tree.list(2,q)[0].id==1,"exclusive cursor");
        PostQuery batch;batch.ids={first.id,999999,2};
        auto selected=tree.list(2,batch);check(selected.size()==2&&selected[0].id==2&&selected[1].id==first.id,"bounded batch order and missing IDs");
        batch.ids={0};rejects(Failure::Invalid,[&]{tree.list(2,batch);});
        batch.ids.assign(101,first.id);rejects(Failure::Invalid,[&]{tree.list(2,batch);});
        rejects(Failure::Forbidden,[&]{tree.remove(2,first.id,5000);});
        rejects(Failure::Forbidden,[&]{tree.remove(9,first.id,5000,"内容管理员不能删除他人果实");});
        check(tree.get(1,first.id).canDelete && !tree.get(2,first.id).canDelete,"server action capabilities");
        check(tree.get(10,first.id).deleteReasonRequired,"admin requires reason");
        rejects(Failure::Forbidden,[&]{tree.hide(1,first.id,"举报",5000);});
        tree.hide(9,first.id,"管理员处理举报",5000);
        rejects(Failure::NotFound,[&]{tree.get(1,first.id);});
        check(tree.count(1)==1,"hidden post must not grow tree");
        batch.ids={first.id,2};selected=tree.list(2,batch);check(selected.size()==1&&selected[0].id==2,"hidden batch content filtered");
        tree.remove(1,2,5000); tree.remove(1,2,5000); check(tree.count(1)==0,"delete idempotence");
        // 两个设备同时提交同一键，只能产生一个果实和一条创建事件。
        PublishPost concurrent{"same-device-key","并发",1,{}, {},{}};
        std::exception_ptr failure;
        std::mutex errorMutex;
        auto submit=[&]{ try { tree.publish(1,concurrent,6000); } catch (...) { std::lock_guard lock(errorMutex); failure=std::current_exception(); } };
        std::thread a(submit), b(submit); a.join(); b.join(); if(failure) std::rethrow_exception(failure);
        check(tree.count(1)==1,"concurrent idempotence");
        const auto administrative=tree.list(10,{}).front();
        rejects(Failure::Invalid,[&]{tree.remove(10,administrative.id,6001);});
        const auto beforeDelete=store.events().size();
        store.failNextEvent();rejects(Failure::Busy,[&]{tree.remove(10,administrative.id,6001,"违规内容");});
        check(tree.get(1,administrative.id).state==PostState::Published,"delete and audit roll back together");
        tree.remove(10,administrative.id,6002,"违规内容");tree.remove(10,administrative.id,6003,"违规内容");
        check(store.events().size()==beforeDelete+2 && store.events().back().data.at("action")=="delete","admin delete audit and retry dedup");
        std::cout << "PASS tree contract: transaction, deduplication, ACL, media, pagination, ranking inputs and concurrency\n";
    } catch(const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
