#pragma once
#include <libpq-fe.h>
#include "backend/application/domain/Domain/Domain.h"
#include "backend/application/observability/BusinessMetrics/BusinessMetrics.h"
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

namespace webserver::phase11::storage_detail {
/** Driver-only SQL values. Never included by application services. */
using Parameters = std::vector<std::optional<std::string>>;

class Result final
{
public:
    explicit Result(PGresult *value = nullptr) : value_(value) {}
    ~Result() { if (value_) PQclear(value_); }
    Result(const Result &) = delete;
    Result &operator=(const Result &) = delete;
    Result(Result &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    [[nodiscard]] int rows() const noexcept { return PQntuples(value_); }
    [[nodiscard]] bool isNull(int row, int column) const noexcept
    {
        return PQgetisnull(value_, row, column) != 0;
    }
    [[nodiscard]] std::string value(int row, int column) const
    {
        return isNull(row, column) ? std::string{} : PQgetvalue(value_, row, column);
    }

private:
    PGresult *value_ = nullptr;
};

inline Result execute(PGconn *connection, std::string_view sql,
               const Parameters &parameters, ExecStatusType expected)
{
    webserver::business_metrics::Timer timer{webserver::business_metrics::Database};
    std::vector<const char *> values;
    values.reserve(parameters.size());
    for (const auto &parameter : parameters)
        values.push_back(parameter ? parameter->c_str() : nullptr);

    PGresult *raw = PQexecParams(
        connection, std::string(sql).c_str(), static_cast<int>(values.size()),
        nullptr, values.empty() ? nullptr : values.data(), nullptr, nullptr, 0);
    if (!raw)
        throw std::runtime_error("libpq returned no result: " +
                                 std::string(PQerrorMessage(connection)));
    if (PQresultStatus(raw) != expected)
    {
        const std::string error = PQresultErrorMessage(raw);
        PQclear(raw);
        throw std::runtime_error("PostgreSQL statement failed: " + error);
    }
    return Result(raw);
}

inline Result query(PGconn *connection, std::string_view sql,
             const Parameters &parameters = {})
{
    return execute(connection, sql, parameters, PGRES_TUPLES_OK);
}

inline void command(PGconn *connection, std::string_view sql,
             const Parameters &parameters = {})
{
    (void)execute(connection, sql, parameters, PGRES_COMMAND_OK);
}

inline std::string number(std::uint64_t value) { return std::to_string(value); }

inline std::string encodeTime(TimePoint value)
{
    return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                              value.time_since_epoch()).count());
}

inline TimePoint decodeTime(const Result &result, int row, int column)
{
    return TimePoint{std::chrono::microseconds{
        std::stoll(result.value(row, column))}};
}

inline std::optional<TimePoint> decodeOptionalTime(const Result &result,
                                            int row, int column)
{
    return result.isNull(row, column)
               ? std::nullopt
               : std::optional<TimePoint>{decodeTime(result, row, column)};
}

inline std::optional<std::uint64_t> decodeOptionalId(const Result &result,
                                              int row, int column)
{
    return result.isNull(row, column)
               ? std::nullopt
               : std::optional<std::uint64_t>{
                     std::stoull(result.value(row, column))};
}

inline bool decodeBool(const Result &result, int row, int column)
{
    return result.value(row, column) == "t";
}
}
