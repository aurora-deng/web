#include "backend/application/modules/shared/infrastructure/Phase11Bridge/Phase11Bridge.h"
#include "backend/application/modules/tree/infrastructure/TreePostgresRepository/TreePostgresRepository.h"
#include "backend/application/modules/tree/infrastructure/TreeRankingSource/TreeRankingSource.h"
#include "backend/application/modules/tree/application/TreeService/TreeService.h"
#include "backend/application/modules/admin/infrastructure/AdminPostgresRepository/AdminPostgresRepository.h"
#include "backend/application/modules/admin/application/AdminService/AdminService.h"
#include "backend/application/modules/announcement/infrastructure/AnnouncementPostgresRepository/AnnouncementPostgresRepository.h"
#include "backend/application/modules/announcement/application/AnnouncementService/AnnouncementService.h"
#include "backend/application/modules/ranking/infrastructure/RankingPostgresRepository/RankingPostgresRepository.h"
#include "backend/application/modules/ranking/application/RankingService/RankingService.h"
#include "backend/application/modules/messaging/infrastructure/PostgresEventWriter/PostgresEventWriter.h"
#include "backend/application/modules/messaging/infrastructure/PostgresEventPipeline/PostgresEventPipeline.h"
#include "backend/application/modules/messaging/application/MessagingService/MessagingService.h"
#include "backend/ai/speech/infrastructure/SpeechPostgresTasks/SpeechPostgresTasks.h"
#include "backend/application/transport/FlatJson/FlatJson.h"
#include "backend/application/storage/postgres/PostgresStore/PostgresStore.h"
#include "backend/application/modules/messaging/infrastructure/ChatOutboxWriter/ChatOutboxWriter.h"
#include <cstdlib>
#include <iostream>
#include <thread>
using namespace webserver::function3;
void check(bool x,const char*text){if(!x)throw std::runtime_error(text);}
template<class F>void rejects(Failure expected,F f){try{f();}catch(const BusinessError&e){check(e.code==expected,"wrong failure");return;}throw std::runtime_error("operation unexpectedly accepted");}
struct FakeBroker:IEventBroker{bool available{true};std::vector<BrokerMessage>messages;bool publish(const BrokerMessage&m)override{if(!available)return false;messages.push_back(m);return true;}};
int main(){try{
    const auto *connection=std::getenv("PHASE11_POSTGRES_CONNECTION");if(!connection)throw std::runtime_error("test connection required");
    SqlContext sql({connection,6,std::chrono::milliseconds(2000)});
    check(sql.query("SELECT current_database()")[0][0].ends_with("_test"),"refusing cleanup outside _test database");
    sql.query("TRUNCATE function3_inbox,function3_outbox,function3_delivery_failures,tree_posts,tree_rank_versions,site_audit RESTART IDENTITY CASCADE");
    auto user=[&](std::string name){return std::stoull(sql.query("INSERT INTO users(username,display_name,password_hash) VALUES($1,$1,'test-only') ON CONFLICT(username) DO UPDATE SET disabled=FALSE RETURNING id",{std::move(name)})[0][0]);};
    const Id a=user("f3_author"),b=user("f3_reader"),admin=user("f3_admin");
    const Id ai=user("f3_ai");sql.query("UPDATE users SET ai_account=TRUE WHERE id=$1",{std::to_string(ai)});
    sql.query("INSERT INTO site_roles VALUES($1,'administrator') ON CONFLICT(user_id) DO UPDATE SET role='administrator'",{std::to_string(admin)});
    const Id category=std::stoull(sql.query("SELECT id FROM tree_categories WHERE active ORDER BY id LIMIT 1")[0][0]);
    Phase11Bridge bridge(sql);TreePostgresRepository repo(sql);PostgresEventWriter events(sql);TreeService tree(sql,bridge,repo,bridge,events);
    RankingPostgresRepository rankRepo(sql);TreeRankingSource source(sql);AnnouncementPostgresRepository annRepo(sql);AnnouncementService ann(sql,bridge,annRepo,events);RankingService rank(sql,rankRepo,source,ann);
    AdminPostgresRepository admins(sql);AdminService accounts(sql,bridge,admins);
    const auto now=rankRepo.databaseNow();PublishPost request{"pg-post","数据库中的第一枚果实",category,"平静",{"测试"},{}};
    const auto p=tree.publish(a,request,now-1000);
    check(tree.publish(a,request,now).id==p.id,"persistent idempotency");
    auto bad=request;bad.body="different";rejects(Failure::Conflict,[&]{tree.publish(a,bad,now);});
    check(tree.like(a,p.id,true,now).likes==0,"self excluded");check(tree.like(b,p.id,true,now).likes==1,"other like");
    auto reply=tree.reply(b,p.id,"pg-reply","祝你开心",now);check(tree.reply(b,p.id,"pg-reply","祝你开心",now).id==reply.id,"reply idempotency");
    tree.reply(b,p.id,"pg-reply-2","再来一条",now);check(tree.get(b,p.id).repliers==1,"distinct repliers");
    {
        const auto before=sql.snapshot().acquires;
        const auto page=repo.posts(PostQuery{},b);
        check(sql.snapshot().acquires-before==3,"list hydration must use three SQL calls independent of page size");
        check(page.size()==1&&page[0].id==p.id&&page[0].likes==1&&page[0].repliers==1&&page[0].likedByViewer,"batch scalar aggregates preserve viewer semantics");
        check(page[0].content.tags==request.tags,"batch tags preserve content");
        PostQuery batch;batch.ids={p.id,999999};check(repo.posts(batch,b).size()==1,"parameterized post IDs");
    }
    check(source.topCandidates(now)[0].post==p.id,"rank source");
    check(heat({1,"",now-604800000,1,1},now)<0,"seven day exclusive boundary");
    check(rank.settle(),"first rank baseline");check(ann.recent(b,0).empty(),"baseline stays silent");
    // 同一结算槽不重复保存或播报。
    auto version=rank.latest().version;check(!rank.settle()&&rank.latest().version==version,"settlement dedup");
    ann.settings(b,{true,"zm_009"});check(ann.settings(b).voice=="zm_009","notification persistence");
    rejects(Failure::Forbidden,[&]{accounts.role(a,b,Role::Administrator,"not allowed");});
    rejects(Failure::Invalid,[&]{accounts.role(admin,ai,Role::Administrator,"AI cannot rescue a human login");});
    rejects(Failure::Conflict,[&]{accounts.role(admin,admin,Role::User,"last admin");});
    rejects(Failure::Conflict,[&]{accounts.active(admin,admin,false,"last admin");});
    accounts.role(admin,b,Role::Moderator,"contract test");check(accounts.identity(b).role==Role::Moderator,"site role");
    tree.hide(b,p.id,"违规内容",now);rejects(Failure::NotFound,[&]{tree.get(a,p.id);});check(tree.count(a)==0,"hidden fruit count");
    check(source.topCandidates(now).empty(),"hidden fruit excluded from ranks");
    // 故障事件不会挡住另一个 aggregate；同一 aggregate 后继必须等待。
    int committed=0;
    sql.transact([&]{sql.afterCommit([&]{++committed;});check(committed==0,"notification before commit");});
    check(committed==1,"commit notification missing");
    try{sql.transact([&]{sql.afterCommit([&]{++committed;});throw std::runtime_error("rollback");});}catch(const std::runtime_error&){}
    check(committed==1,"rollback must discard notification");
    FakeBroker broker;PostgresEventPipeline pipeline(sql);
    broker.available=false;check(pipeline.publishOne(broker),"failed publish retained");
    auto pending=sql.query("SELECT state,attempts FROM function3_outbox ORDER BY id LIMIT 1");check(pending[0][0]=="pending"&&pending[0][1]=="1","retry accounting");
    sql.transact([&]{events.appendEvent({0,"tree.post.created","tree.post",999,1,{{"postId","999"}},now});});
    broker.available=true;check(pipeline.publishOne(broker),"independent aggregate progresses");check(broker.messages.size()==1,"single accepted event");
    int notifications=0;check(pipeline.consume(broker.messages[0],[&](auto,auto){++notifications;}),"consumer commit");
    check(pipeline.consume(broker.messages[0],[&](auto,auto){++notifications;})&&notifications==1&&pipeline.duplicates()==1,"persistent inbox dedup");
    // 消费者在持久副作用中失败，Inbox 必须一起回滚，下一次才能真正处理。
    auto message=broker.messages[0];auto fields=webserver::phase11::transport::parseFlatStringObject(message.body);
    const auto originalId=fields.at("id");
    message.body.replace(message.body.find("\"id\":\""+originalId+"\""),originalId.size()+7,"\"id\":\"9999999\"");
    check(!pipeline.consume(message,[](auto,auto){},[](auto&){throw std::runtime_error("consumer crash before commit");}),"consumer failure must NACK");
    check(sql.query("SELECT 1 FROM function3_inbox WHERE event_id=9999999").empty(),"Inbox rolled back with consumer fault");
    check(!pipeline.failures(0).empty(),"consumer failure visible to operations");
    check(pipeline.consume(message,[](auto,auto){}),"consumer redelivery succeeds");
    check(pipeline.failures(0).empty(),"successful redelivery resolves failure metadata");
    const auto delivery=sql.query("INSERT INTO function3_outbox(event_type,aggregate_type,aggregate_id,aggregate_version,payload,created_ms,state) VALUES('tree.post.updated','tree.post',888,1,'{}',0,'published') RETURNING id")[0][0];
    fields["id"]=delivery;fields["type"]="tree.post.updated";fields["aggregateId"]="888";
    message.key="function3:"+delivery;message.body="{";
    for(auto&[key,value]:fields){if(message.body.size()>1)message.body+=",";message.body+=webserver::phase11::transport::quoteJson(key)+":"+webserver::phase11::transport::quoteJson(value);}message.body+="}";
    check(!pipeline.consume(message,[](auto,auto){},[](auto&){throw std::runtime_error("consumer failure");}),"failed consumer NACK");
    MessagingService deliveryOps(sql,bridge,pipeline,events);
    rejects(Failure::Forbidden,[&]{deliveryOps.failures(a,0);});
    deliveryOps.replay(admin,std::stoull(delivery),"replay a published but failed delivery",now+10);
    check(sql.query("SELECT state FROM function3_outbox WHERE id=$1",{delivery})[0][0]=="pending","published consumer failure can be republished with audit");
    check(pipeline.consume(message,[](auto,auto){}),"consumer replay success");
    // 将唯一的失败 aggregate 推至八次，验证死信与带权限/审计的重放。
    const auto failed=sql.query("SELECT id FROM function3_outbox WHERE state='pending' ORDER BY id LIMIT 1")[0][0];
    broker.available=false;
    for(int i=1;i<8;++i){sql.query("UPDATE function3_outbox SET available_at=CURRENT_TIMESTAMP-INTERVAL '1 second' WHERE id=$1",{failed});check(pipeline.publishOne(broker),"retry attempt");}
    check(sql.query("SELECT state FROM function3_outbox WHERE id=$1",{failed})[0][0]=="dead","eight failures become dead");
    MessagingService ops(sql,bridge,pipeline,events);rejects(Failure::Forbidden,[&]{ops.replay(a,std::stoull(failed),"denied",now);});
    ops.replay(admin,std::stoull(failed),"contract recovery",now);
    check(sql.query("SELECT state,attempts FROM function3_outbox WHERE id=$1",{failed})[0]==std::vector<std::string>({"pending","0"}),"audited replay resets retry budget");
    check(!sql.query("SELECT 1 FROM site_audit WHERE action='replay'").empty(),"replay audit persisted");
    // TTS 失败只耗自己的有限重试预算，不改变文字公告。
    Id announcement{};sql.transact([&]{announcement=annRepo.create(version,"speech failure contract").id;});
    SpeechPostgresTasks speech(sql);sql.transact([&]{speech.enqueue(announcement,"测试语音失败不影响文字",now+300000);});
    for(int i=0;i<3;++i){auto job=speech.claim();check(job.has_value(),"speech claim");speech.failed(*job);sql.query("UPDATE announcement_audio SET available_at=CURRENT_TIMESTAMP-INTERVAL '1 second' WHERE announcement_id=$1 AND voice=$2",{std::to_string(job->announcement),job->voice});}
    check(sql.query("SELECT count(*) FROM announcement_audio WHERE state='failed'")[0][0]=="1","speech retry stops at three");
    sql.query("UPDATE announcement_audio SET expires_ms=0 WHERE state='pending'");
    check(!speech.claim(),"expired speech must not generate");
    check(!annRepo.recent(0,20).empty(),"speech failure preserves text");
    webserver::phase11::PostgresStore legacy({connection,1,std::chrono::milliseconds(2000)});
    int handedOff=0;legacy.setOutboxWriter(std::make_shared<ChatOutboxWriter>([&]{++handedOff;}));
    auto clock=std::chrono::system_clock::now();
    auto transaction=legacy.beginTransaction();
    auto mirrored=legacy.appendOutbox({0,"mirror.contract","user",a,"atomic handoff",clock,{}});
    check(sql.query("SELECT id FROM function3_outbox WHERE event_type='chat.legacy' AND aggregate_version=$1",{std::to_string(mirrored.id)}).empty(),"handoff visible before commit");
    transaction->commit();transaction.reset();
    check(handedOff==1,"handoff notification missing after commit");
    check(sql.query("SELECT id FROM outbox_events WHERE id=$1 AND published_at IS NOT NULL",{std::to_string(mirrored.id)}).size()==1,"legacy handoff completion persisted");
    check(sql.query("SELECT id FROM function3_outbox WHERE event_type='chat.legacy' AND aggregate_type='chat.user' AND aggregate_version=$1",{std::to_string(mirrored.id)}).size()==1,"durable queue committed with legacy event");
    transaction=legacy.beginTransaction();auto rolledBack=legacy.appendOutbox({0,"mirror.contract","user",a,"rollback",clock,{}});transaction->rollback();transaction.reset();
    check(sql.query("SELECT id FROM function3_outbox WHERE event_type='chat.legacy' AND aggregate_version=$1",{std::to_string(rolledBack.id)}).empty(),"rollback leaked queue event");
    check(sql.query("SELECT id FROM outbox_events WHERE id=$1",{std::to_string(rolledBack.id)}).empty(),"rollback leaked legacy event");
    check(handedOff==1,"rollback emitted a handoff notification");
    std::cout<<"Function3 PostgreSQL: tree, rank, admin, settings, retry and Inbox passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
