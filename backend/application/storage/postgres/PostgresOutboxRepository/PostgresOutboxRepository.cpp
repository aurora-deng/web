#include <unordered_set>
#include "backend/application/storage/postgres/PostgresOutboxRepository/PostgresOutboxRepository.h"
#include "backend/application/storage/postgres/PostgresValueCodec/PostgresValueCodec.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webserver::phase11 {
namespace {
using namespace storage_detail;
OutboxEvent readOutbox(const Result &result, int row)
{
    return {std::stoull(result.value(row, 0)), result.value(row, 1),
            result.value(row, 2), std::stoull(result.value(row, 3)),
            result.value(row, 4), decodeTime(result, row, 5),
            decodeOptionalTime(result, row, 6)};
}

constexpr std::string_view kOutboxColumns =
    "id, event_type, aggregate_type, aggregate_id, payload, "
    "(extract(epoch FROM created_at) * 1000000)::bigint, "
    "(extract(epoch FROM published_at) * 1000000)::bigint";
}
OutboxEvent PostgresOutboxRepository::appendOutbox(PGconn *connection, bool ownsTransaction, const std::shared_ptr<PostgresOutboxWriter>& writer, OutboxEvent event)
{

    if(writer){auto written=writer->append(connection,std::move(event));if(ownsTransaction)writer->committed();return written;}
    const auto result = query(connection,
        "INSERT INTO outbox_events(event_type,aggregate_type,aggregate_id,payload,created_at) "
        "VALUES($1,$2,$3::bigint,$4,to_timestamp($5::double precision/1000000.0)) RETURNING " +
        std::string(kOutboxColumns),
        {event.type, event.aggregateType, number(event.aggregateId), event.payload,
         encodeTime(event.createdAt)});
    return readOutbox(result, 0);
}

std::vector<OutboxEvent> PostgresOutboxRepository::pendingOutbox(PGconn *connection, std::size_t limit)
{

    const auto result = query(connection,
        "SELECT " + std::string(kOutboxColumns) +
        " FROM outbox_events WHERE published_at IS NULL ORDER BY id LIMIT $1::bigint",
        {number(limit)});
    std::vector<OutboxEvent> events;
    events.reserve(result.rows());
    for (int row = 0; row < result.rows(); ++row)
        events.push_back(readOutbox(result, row));
    return events;
}

bool PostgresOutboxRepository::markPublished(PGconn *connection, OutboxEventId id, TimePoint now)
{

    return query(connection,
        "UPDATE outbox_events SET published_at=to_timestamp($2::double precision/1000000.0) "
        "WHERE id=$1::bigint RETURNING 1", {number(id), encodeTime(now)}).rows() == 1;
}

}

namespace webserver::phase11 {
bool PostgresOutboxRepository::markPublishedBatch(PGconn *connection,const std::vector<OutboxEventId> &ids,TimePoint now) {
    if(ids.empty())return true;
    if(ids.size()>1000 || std::unordered_set<OutboxEventId>(ids.begin(),ids.end()).size()!=ids.size())return false;
    std::string array="{";
    for(auto id:ids){if(array.size()>1)array+=",";array+=number(id);}array+="}";
    const auto result=query(connection,
        "WITH selected AS (SELECT id FROM outbox_events WHERE id=ANY($1::bigint[])),"
        "updated AS (UPDATE outbox_events SET published_at=to_timestamp($2::double precision/1000000.0) "
        "WHERE id IN (SELECT id FROM selected) AND (SELECT count(*) FROM selected)=cardinality($1::bigint[]) "
        "RETURNING id) SELECT count(*) FROM updated",{array,encodeTime(now)});
    return std::stoull(result.value(0,0))==ids.size();
}
}
