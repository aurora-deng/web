#include "PostgresEventPipeline.h"
#include "backend/application/transport/FlatJson/FlatJson.h"
namespace webserver::function3 {
namespace {
using phase11::transport::quoteJson;
std::string envelope(const SqlRows::value_type&r){return "{\"id\":"+quoteJson(r[0])+",\"type\":"+quoteJson(r[1])+",\"aggregate\":"+quoteJson(r[2])+",\"aggregateId\":"+quoteJson(r[3])+",\"version\":"+quoteJson(r[4])+",\"payload\":"+quoteJson(r[5])+"}";}
}
bool PostgresEventPipeline::publishOne(IEventBroker &broker){
    return publishBatch(broker,1)!=0;
}
std::size_t PostgresEventPipeline::publishBatch(IEventBroker &broker,std::size_t limit){
    if(!limit||limit>4)throw std::invalid_argument("publisher batch limit must be 1..4");
    SqlRows rows;
    sql_.transact([&]{
        rows=sql_.query("WITH picked AS (SELECT o.id FROM function3_outbox o WHERE ((o.state='pending' AND o.available_at<=CURRENT_TIMESTAMP) OR (o.state='inflight' AND o.lease_until<CURRENT_TIMESTAMP)) "
            "AND NOT EXISTS(SELECT 1 FROM function3_outbox previous WHERE previous.aggregate_type=o.aggregate_type AND previous.aggregate_id=o.aggregate_id AND previous.id<o.id AND previous.state<>'published') "
            "ORDER BY o.id FOR UPDATE SKIP LOCKED LIMIT $1) UPDATE function3_outbox o SET state='inflight',attempts=attempts+1,lease_until=CURRENT_TIMESTAMP+INTERVAL '45 seconds' FROM picked WHERE o.id=picked.id "
            "RETURNING o.id,o.event_type,o.aggregate_type,o.aggregate_id,o.aggregate_version,o.payload::text,o.attempts",{std::to_string(limit)});
    });
    if(rows.empty())return 0;
    std::vector<bool>accepted;
    // 一批最多四个不同 aggregate 的队首；同会话仍不会越序。合并领取和确认事务，
    // 减少同步落盘次数。8秒发送上限×4小于45秒租约，网络等待不持有数据库连接。
    for(auto&r:rows){const bool speech=r[1]=="speech.requested";bool ok=false;
        try{ok=broker.publish({speech?"Function3Speech":r[1]=="chat.legacy"?"Function3Chat":"Function3Tree",r[2]+":"+r[3],"function3:"+r[0],envelope(r),!speech});}catch(...){}
        accepted.push_back(ok);
    }
    sql_.transact([&]{for(std::size_t i=0;i<rows.size();++i){auto&r=rows[i];const bool ok=accepted[i];sql_.query("UPDATE function3_outbox SET state=$2,lease_until=NULL,last_error=$3,available_at=CURRENT_TIMESTAMP+make_interval(secs=>LEAST(60,power(2,LEAST(attempts,6))::int)) WHERE id=$1 AND state='inflight'",
        {r[0],ok?"published":std::stoul(r[6])>=8?"dead":"pending",ok?"":"broker rejected or timed out"});}});
    return rows.size();
}
bool PostgresEventPipeline::consume(const BrokerMessage&m,const std::function<void(std::string,std::string)>&notify,const std::function<void(const std::map<std::string,std::string>&)>&durable){
    if(m.body.size()>65536)return false;
    std::string eventId="0";
    try {
        const auto f=phase11::transport::parseFlatStringObject(m.body);
        for(const auto*field:{"id","type","aggregate","aggregateId","version","payload"})if(!f.contains(field))throw std::invalid_argument("incomplete event");
        const auto id=f.at("id");
        if(id.empty()||id.size()>19||id.find_first_not_of("0123456789")!=std::string::npos||!std::stoull(id)||std::stoull(id)>9223372036854775807ULL)throw std::invalid_argument("invalid event id");
        eventId=id;
        const std::string consumer=m.topic=="Function3Chat"?"chat.realtime":"function3.delivery";
        bool inserted=false;
        sql_.transact([&]{
            inserted=!sql_.query("INSERT INTO function3_inbox(consumer,event_id) VALUES($1,$2) ON CONFLICT DO NOTHING RETURNING event_id",{consumer,id}).empty();
            if(inserted&&durable)durable({f.begin(),f.end()});
            sql_.query("UPDATE function3_delivery_failures SET state='resolved' WHERE topic=$1 AND broker_key=$2",{m.topic,m.key.substr(0,256)});
        });
        if(!inserted){++duplicates_;return true;}
        ++accepted_;
        // 在线提示允许丢失，持久消息/公告由 HTTP 游标补发。MQ ACK 不表示浏览器 ACK。
        try{notify(f.at("type"),m.body);}catch(...){}return true;
    } catch(...) {
        // 失败事务已回滚；独立记录运维元数据，不保存用户正文，不假装已经处理成功。
        // 数据库完全不可用时，此记录也可能失败，仍返回 NACK，让 Broker 保留重投责任。
        try{sql_.transact([&]{sql_.query("INSERT INTO function3_delivery_failures(topic,broker_key,event_id,last_error) VALUES($1,$2,$3,'consumer transaction or envelope failed') ON CONFLICT(topic,broker_key) DO UPDATE SET attempts=function3_delivery_failures.attempts+1,state='failed',failed_at=CURRENT_TIMESTAMP,event_id=EXCLUDED.event_id",{m.topic,m.key.substr(0,256),eventId});});}catch(...){}
        return false;
    }
}

bool PostgresEventPipeline::retry(Id event){sql_.requireTransaction();return !sql_.query("UPDATE function3_outbox SET state='pending',attempts=0,available_at=CURRENT_TIMESTAMP,last_error='' WHERE id=$1 AND (state='dead' OR (state='published' AND EXISTS(SELECT 1 FROM function3_delivery_failures f WHERE f.event_id=$1 AND f.state='failed'))) RETURNING id",{std::to_string(event)}).empty();}
std::vector<QueueState>PostgresEventPipeline::status()const{std::vector<QueueState>result;for(auto&r:sql_.query("SELECT state,count(*),COALESCE(min(id),0) FROM function3_outbox GROUP BY state ORDER BY state"))result.push_back({r[0],std::stoull(r[1]),std::stoull(r[2])});return result;}
std::vector<DeadEvent>PostgresEventPipeline::dead(Id before)const{std::vector<DeadEvent>result;for(auto&r:sql_.query("SELECT id,event_type,aggregate_type,last_error,aggregate_id,attempts FROM function3_outbox WHERE state='dead' AND ($1::bigint=0 OR id<$1) ORDER BY id DESC LIMIT 50",{std::to_string(before)}))result.push_back({std::stoull(r[0]),r[1],r[2],r[3],std::stoull(r[4]),std::stoull(r[5])});return result;}
std::vector<DeliveryFailure>PostgresEventPipeline::failures(Id before)const{
    std::vector<DeliveryFailure>result;
    for(auto&r:sql_.query("SELECT id,event_id,topic,broker_key,last_error,attempts FROM function3_delivery_failures WHERE state='failed' AND ($1::bigint=0 OR id<$1) ORDER BY id DESC LIMIT 50",{std::to_string(before)}))result.push_back({std::stoull(r[0]),std::stoull(r[1]),r[2],r[3],r[4],std::stoull(r[5])});
    return result;
}

bool ChatMqSink::publish(const phase11::OutboxEvent &e){
    auto body="{\"id\":"+quoteJson(std::to_string(e.id))+",\"type\":"+quoteJson(e.type)+",\"aggregate\":"+quoteJson(e.aggregateType)+",\"aggregateId\":"+quoteJson(std::to_string(e.aggregateId))+",\"payload\":"+quoteJson(e.payload)+"}";
    Id conversation=e.aggregateId;
    const std::string aggregate=(e.aggregateType=="message"||e.aggregateType=="conversation")?"chat.conversation":"chat."+e.aggregateType;
    if(e.aggregateType=="message"){
        auto message=messages_.findMessage(e.aggregateId);if(!message)return false;
        conversation=message->conversationId;
    }
    // 旧 Outbox 的搬运先落到持久发布队列，不在全局 Outbox 头部等待 Broker。
    // 同会话按旧事件 ID 排序；跨会话可以独立重试/死信，不再被一个坏事件拖住。
    sql_.transact([&]{sql_.query("INSERT INTO function3_outbox(event_type,aggregate_type,aggregate_id,aggregate_version,payload,created_ms) VALUES('chat.legacy',$1,$2,$3,$4::jsonb,$5) ON CONFLICT DO NOTHING",
        {aggregate,std::to_string(conversation),std::to_string(e.id),body,std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(e.createdAt.time_since_epoch()).count())});if(committed_)sql_.afterCommit(committed_);});
    return true;
}
}
