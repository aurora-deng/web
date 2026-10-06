#include "server/phase11/storage/postgres/PostgresConnectionPool/PostgresConnectionPool.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace webserver::phase11
{

PostgresConnectionPool::Lease::~Lease()
{
    reset();
}

PostgresConnectionPool::Lease::Lease(Lease &&other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)),
      connection_(std::exchange(other.connection_, nullptr))
{
}

PostgresConnectionPool::Lease &PostgresConnectionPool::Lease::operator=(
    Lease &&other) noexcept
{
    if (this != &other)
    {
        reset();
        pool_ = std::exchange(other.pool_, nullptr);
        connection_ = std::exchange(other.connection_, nullptr);
    }
    return *this;
}

void PostgresConnectionPool::Lease::reset() noexcept
{
    if (pool_ && connection_)
        pool_->release(connection_);
    pool_ = nullptr;
    connection_ = nullptr;
}

PostgresConnectionPool::PostgresConnectionPool(PostgresPoolConfig config)
    : config_(std::move(config))
{
    if (config_.connectionString.empty() || config_.size == 0 ||
        config_.acquireTimeout <= std::chrono::milliseconds::zero())
        throw std::invalid_argument("invalid PostgreSQL pool configuration");

    all_.reserve(config_.size);
    available_.reserve(config_.size);
    try
    {
        for (std::size_t index = 0; index < config_.size; ++index)
        {
            PGconn *connection = PQconnectdb(config_.connectionString.c_str());
            if (!connection)
                throw std::runtime_error("libpq could not allocate a connection");
            if (PQstatus(connection) != CONNECTION_OK)
            {
                const std::string error = PQerrorMessage(connection);
                PQfinish(connection);
                throw std::runtime_error("PostgreSQL connection failed: " + error);
            }
            all_.push_back(connection);
            available_.push_back(connection);
        }
    }
    catch (...)
    {
        for (auto *connection : all_)
            PQfinish(connection);
        all_.clear();
        available_.clear();
        throw;
    }
}

PostgresConnectionPool::~PostgresConnectionPool()
{
    std::unique_lock lock(mutex_);
    stopping_ = true;
    availableChanged_.wait(lock, [this] {
        return available_.size() == all_.size();
    });
    const auto connections = std::move(all_);
    available_.clear();
    lock.unlock();
    for (auto *connection : connections)
        PQfinish(connection);
}

PostgresConnectionPool::Lease PostgresConnectionPool::acquire()
{
    const auto started = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex_);
    const bool waited = available_.empty() && !stopping_;
    if (waited)
        ++waitedAcquires_;
    if (!availableChanged_.wait_for(lock, config_.acquireTimeout, [this] {
            return stopping_ || !available_.empty();
        }))
    {
        ++timeouts_;
        throw std::runtime_error("PostgreSQL connection pool exhausted");
    }
    if (stopping_)
        throw std::runtime_error("PostgreSQL connection pool is stopping");
    auto *connection = available_.back();
    available_.pop_back();
    ++acquires_;
    const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    totalAcquireWaitMicroseconds_ += static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, wait));
    inUseHighWatermark_ = std::max(
        inUseHighWatermark_, all_.size() - available_.size());
    return Lease(this, connection);
}

PostgresConnectionPool::Snapshot PostgresConnectionPool::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    return {all_.size(), available_.size(), all_.size() - available_.size(),
            inUseHighWatermark_, acquires_, waitedAcquires_, timeouts_,
            totalAcquireWaitMicroseconds_};
}

void PostgresConnectionPool::restoreIdleConnection(PGconn *connection) noexcept
{
    if (PQstatus(connection) != CONNECTION_OK)
        PQreset(connection);
    if (PQstatus(connection) == CONNECTION_OK &&
        PQtransactionStatus(connection) != PQTRANS_IDLE)
    {
        if (auto *result = PQexec(connection, "ROLLBACK"))
            PQclear(result);
    }
}

void PostgresConnectionPool::release(PGconn *connection) noexcept
{
    restoreIdleConnection(connection);
    {
        std::lock_guard lock(mutex_);
        available_.push_back(connection);
    }
    availableChanged_.notify_all();
}

} // namespace webserver::phase11
