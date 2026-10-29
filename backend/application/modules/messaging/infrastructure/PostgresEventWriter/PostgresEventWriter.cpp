#include "PostgresEventWriter.h"
#include "backend/application/transport/FlatJson/FlatJson.h"
namespace webserver::function3 {
void PostgresEventWriter::appendEvent(DomainEvent event) {
    sql_.requireTransaction();
    std::string json="{";
    for(auto &[key,value]:event.data){if(json.size()>1)json+=",";json+=phase11::transport::quoteJson(key)+":"+phase11::transport::quoteJson(value);}
    json+="}";
    sql_.query("INSERT INTO function3_outbox(event_type,aggregate_type,aggregate_id,aggregate_version,payload,created_ms) VALUES($1,$2,$3,$4,$5::jsonb,$6)",
        {event.type,event.aggregate,std::to_string(event.aggregateId),std::to_string(event.version),json,std::to_string(event.createdAt)});
    if(event.type=="admin.audit")sql_.query("INSERT INTO site_audit(actor_id,action,resource,reason) VALUES($1,$2,$3,$4)",
        {event.data.at("actorId"),event.data.at("action"),event.aggregate+":"+std::to_string(event.aggregateId),event.data.at("reason")});
    if(committed_)sql_.afterCommit(committed_);
}
}
