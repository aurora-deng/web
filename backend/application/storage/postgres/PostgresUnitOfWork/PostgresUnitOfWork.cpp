#include "PostgresUnitOfWork.h"
#include "backend/application/storage/postgres/PostgresValueCodec/PostgresValueCodec.h"
#include "backend/application/observability/tracing/ChatAckTrace.h"
#include <thread>
#include <unordered_map>
#include <stdexcept>
namespace webserver::phase11 {
namespace {
using storage_detail::command;
thread_local std::unordered_map<const PostgresUnitOfWork*,PGconn*> activeTransactions;
}
class PostgresUnitOfWork::Transaction final : public ITransaction
{
public:
    Transaction(PostgresUnitOfWork &store, PostgresConnectionPool::Lease lease, std::shared_ptr<PostgresOutboxWriter> writer)
        : store_(store), lease_(std::move(lease)), owner_(std::this_thread::get_id()), writer_(std::move(writer))
    {
        const auto [_, inserted] = activeTransactions.emplace(&store_, lease_.get());
        if (!inserted)
            throw std::logic_error("nested PostgreSQL transactions are not supported");
        try
        {
            command(lease_.get(), "BEGIN");
        }
        catch (...)
        {
            activeTransactions.erase(&store_);
            throw;
        }
    }

    ~Transaction() override
    {
        if (!finished_)
        {
            try { rollback(); }
            catch (...) { activeTransactions.erase(&store_); }
        }
    }

    void commit() override { finish("COMMIT"); }
    void rollback() override { finish("ROLLBACK"); }

private:
    void finish(std::string_view action)
    {
        if (finished_)
            throw std::logic_error("transaction already completed");
        if (owner_ != std::this_thread::get_id())
            throw std::logic_error("transaction completed on a different thread");
        try
        {
            const auto started = action == "COMMIT"
                ? application::ChatAckTrace::committing([this] { return PQbackendPID(lease_.get()); })
                : std::chrono::steady_clock::time_point{};
            command(lease_.get(), action);
            application::ChatAckTrace::committed(started);
        }
        catch (...)
        {
            activeTransactions.erase(&store_);
            finished_ = true;
            lease_.reset();
            throw;
        }
        activeTransactions.erase(&store_);
        finished_ = true;
        lease_.reset();
        if(action=="COMMIT"&&writer_)writer_->committed();
    }

    PostgresUnitOfWork &store_;
    PostgresConnectionPool::Lease lease_;
    std::thread::id owner_;
    bool finished_ = false;
    std::shared_ptr<PostgresOutboxWriter> writer_;
};


PostgresUnitOfWork::PostgresUnitOfWork(PostgresPoolConfig config):pool_(std::move(config)){}
bool PostgresUnitOfWork::inTransaction() const {return activeTransactions.contains(this);}
PostgresUnitOfWork::ConnectionScope PostgresUnitOfWork::connection() const {
    if(auto found=activeTransactions.find(this);found!=activeTransactions.end())return {{},found->second};
    ConnectionScope scope;scope.lease.emplace(pool_.acquire());scope.connection=scope.lease->get();return scope;
}
std::unique_ptr<ITransaction> PostgresUnitOfWork::begin(std::shared_ptr<PostgresOutboxWriter> writer){
    // Reject nesting before waiting for a second lease, even in a one-connection pool.
    if(inTransaction())throw std::logic_error("nested PostgreSQL transactions are not supported");
    return std::make_unique<Transaction>(*this,pool_.acquire(),std::move(writer));
}
}
