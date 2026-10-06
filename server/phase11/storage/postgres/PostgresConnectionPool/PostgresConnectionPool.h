#pragma once

#include <libpq-fe.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace webserver::phase11
{

struct PostgresPoolConfig
{
    std::string connectionString;
    std::size_t size = 4;
    std::chrono::milliseconds acquireTimeout = std::chrono::seconds(5);
};

/**
 * libpq 同步连接池。每个 Lease 独占一个 PGconn；因此多个数据库 Worker 可以并行执行，
 * 同一个 PGconn 不会被两个线程同时使用。池满时等待有明确超时，防止请求永久挂起。
 */
class PostgresConnectionPool final
{
public:
    class Lease final
    {
    public:
        Lease() = default;
        ~Lease();

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;

        [[nodiscard]] PGconn *get() const noexcept { return connection_; }
        explicit operator bool() const noexcept { return connection_ != nullptr; }
        void reset() noexcept;

    private:
        friend class PostgresConnectionPool;
        Lease(PostgresConnectionPool *pool, PGconn *connection)
            : pool_(pool), connection_(connection)
        {
        }

        PostgresConnectionPool *pool_ = nullptr;
        PGconn *connection_ = nullptr;
    };

    explicit PostgresConnectionPool(PostgresPoolConfig config);
    ~PostgresConnectionPool();

    PostgresConnectionPool(const PostgresConnectionPool &) = delete;
    PostgresConnectionPool &operator=(const PostgresConnectionPool &) = delete;

    [[nodiscard]] Lease acquire();
    [[nodiscard]] std::size_t size() const noexcept { return all_.size(); }

private:
    void release(PGconn *connection) noexcept;
    static void restoreIdleConnection(PGconn *connection) noexcept;

    PostgresPoolConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable availableChanged_;
    std::vector<PGconn *> all_;
    std::vector<PGconn *> available_;
    bool stopping_ = false;
};

} // namespace webserver::phase11
