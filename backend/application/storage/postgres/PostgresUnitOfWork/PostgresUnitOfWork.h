#pragma once
#include "backend/application/storage/postgres/PostgresConnectionPool/PostgresConnectionPool.h"
#include "backend/application/storage/postgres/PostgresOutboxWriter/PostgresOutboxWriter.h"
#include "backend/application/ports/IDatabase/IDatabase.h"
#include <memory>
#include <optional>
namespace webserver::phase11 {
// Driver-only shared transaction context. Repositories borrow the same lease;
// business data and Outbox must never use independent commits.
class PostgresUnitOfWork final {
public:
    struct ConnectionScope {
        std::optional<PostgresConnectionPool::Lease> lease;
        PGconn* connection=nullptr;
    };
    explicit PostgresUnitOfWork(PostgresPoolConfig config);
    ConnectionScope connection() const;
    bool inTransaction() const;
    std::unique_ptr<ITransaction> begin(std::shared_ptr<PostgresOutboxWriter> writer);
    PostgresConnectionPool::Snapshot snapshot() const noexcept {return pool_.snapshot();}
private:
    class Transaction;
    mutable PostgresConnectionPool pool_;
};
}
