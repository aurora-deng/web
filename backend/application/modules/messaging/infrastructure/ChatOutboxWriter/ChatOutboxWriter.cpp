#include "ChatOutboxWriter.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include <memory>
namespace webserver::function3 {
phase11::OutboxEvent ChatOutboxWriter::append(PGconn*connection,phase11::OutboxEvent event){
    const auto micros=std::chrono::duration_cast<std::chrono::microseconds>(event.createdAt.time_since_epoch()).count();
    const std::string aggregate=std::to_string(event.aggregateId),created=std::to_string(micros);
    const char*values[]={event.type.c_str(),event.aggregateType.c_str(),aggregate.c_str(),event.payload.c_str(),created.c_str()};
    // 同一条语句：旧事件的“已移交”与新队列必须一起成功。外层有业务事务时一起提交；
    // 独立调用时也由 PostgreSQL 单语句事务保证原子性，不能先标记旧事件完成再补写队列。
    constexpr auto sql=
        "WITH original AS (INSERT INTO outbox_events(event_type,aggregate_type,aggregate_id,payload,created_at,published_at) "
        "VALUES($1,$2,$3::bigint,$4,to_timestamp($5::double precision/1000000.0),CURRENT_TIMESTAMP) RETURNING id),"
        "queued AS (INSERT INTO function3_outbox(event_type,aggregate_type,aggregate_id,aggregate_version,payload,created_ms) "
        "SELECT 'chat.legacy',CASE WHEN $2 IN ('message','conversation') THEN 'chat.conversation' ELSE 'chat.'||$2 END,"
        "CASE WHEN $2='message' THEN (SELECT conversation_id FROM messages WHERE id=$3::bigint) ELSE $3::bigint END,original.id,"
        "jsonb_build_object('id',original.id::text,'type',$1::text,'aggregate',$2::text,'aggregateId',$3::text,'payload',$4::text),"
        "$5::bigint/1000 FROM original RETURNING id) SELECT original.id FROM original CROSS JOIN queued";
    std::unique_ptr<PGresult,decltype(&PQclear)>result(PQexecParams(connection,sql,5,nullptr,values,nullptr,nullptr,0),PQclear);
    if(!result||PQresultStatus(result.get())!=PGRES_TUPLES_OK||PQntuples(result.get())!=1)
        throw phase11::ApplicationError(phase11::ErrorCode::Unavailable,"durable event queue write failed");
    event.id=std::stoull(PQgetvalue(result.get(),0,0));event.publishedAt=std::chrono::system_clock::now();return event;
}
}
