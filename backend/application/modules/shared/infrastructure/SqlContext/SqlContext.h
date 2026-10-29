#pragma once
#include "backend/application/modules/shared/BusinessContext/BusinessContext.h"
#include "backend/application/storage/postgres/PostgresConnectionPool/PostgresConnectionPool.h"
#include <vector>
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"

namespace webserver::function3 {
using SqlRows = std::vector<std::vector<std::string>>;
/** 驱动内部复用连接和参数化查询；PG 类型不进入业务端口。 */
class SqlContext final : public IUnitOfWork {
public:
    explicit SqlContext(phase11::PostgresPoolConfig config);
    auto snapshot()const{return pool_.snapshot();}
    void transact(const std::function<void()> &operation) override;
    SqlRows query(const std::string &sql, const std::vector<std::string> &parameters = {}) const;
    bool inTransaction() const;
    void requireTransaction() const;
    /** 驱动自己的提交后提示。回滚时丢弃，提示失败不撤销已经提交的业务。 */
    void afterCommit(std::function<void()> notification);
    /** 仅驱动适配器可调用；复用已有OutboxWriter并保持同一个事务连接。 */
    void appendLegacyEvent(phase11::PostgresOutboxWriter&writer,phase11::OutboxEvent event);
private:
    mutable phase11::PostgresConnectionPool pool_;
};
}
