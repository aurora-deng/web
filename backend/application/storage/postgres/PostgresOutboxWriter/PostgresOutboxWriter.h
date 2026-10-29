#pragma once
#include "backend/application/domain/Domain/Domain.h"
#include <libpq-fe.h>
namespace webserver::phase11 {
/** 存储驱动内部的写入策略。只借用当前事务连接，不拥有连接、不自行提交。
 * PostgreSQL 类型留在 infrastructure，公共业务 Port 不包含它。
 */
class PostgresOutboxWriter {
public:
    virtual ~PostgresOutboxWriter()=default;
    virtual OutboxEvent append(PGconn*connection,OutboxEvent event)=0;
    virtual void committed()noexcept{}
};
}
