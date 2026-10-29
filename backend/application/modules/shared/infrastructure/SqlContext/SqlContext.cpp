#include "SqlContext.h"
#include <map>
#include <memory>
#include <iostream>

namespace webserver::function3 {
namespace {
thread_local std::map<const SqlContext *, PGconn *> connections;
thread_local std::map<const SqlContext *,std::vector<std::function<void()>>> notifications;
SqlRows execute(PGconn *connection, const std::string &sql, const std::vector<std::string> &parameters) {
    std::vector<const char *> values;
    for (const auto &p : parameters) values.push_back(p.c_str());
    std::unique_ptr<PGresult, decltype(&PQclear)> result(PQexecParams(connection, sql.c_str(),
        static_cast<int>(values.size()), nullptr, values.data(), nullptr, nullptr, 0), PQclear);
    if (!result || (PQresultStatus(result.get()) != PGRES_TUPLES_OK && PQresultStatus(result.get()) != PGRES_COMMAND_OK)) {
        // SQL 文本与连接信息不会返回浏览器；详细诊断只能进入受控运维日志。
        const char *state = result ? PQresultErrorField(result.get(), PG_DIAG_SQLSTATE) : nullptr;
        if (state && std::string_view(state) == "23505") throw BusinessError(Failure::Conflict, "resource already exists");
        std::clog<<"Function3 database operation failed, SQLSTATE="<<(state?state:"connection")<<'\n';
        throw BusinessError(Failure::Busy, "database operation failed");
    }
    SqlRows rows;
    for (int r=0; r<PQntuples(result.get()); ++r) {
        std::vector<std::string> row;
        for (int c=0; c<PQnfields(result.get()); ++c) row.emplace_back(PQgetvalue(result.get(),r,c));
        rows.push_back(std::move(row));
    }
    return rows;
}
}
SqlContext::SqlContext(phase11::PostgresPoolConfig config):pool_(std::move(config)){
    // 逐一独占所有连接设置会话级默认值，事务外的查询也有期限；不改数据库全局配置。
    std::vector<phase11::PostgresConnectionPool::Lease>leases;
    for(std::size_t i=0;i<pool_.size();++i){leases.push_back(pool_.acquire());execute(leases.back().get(),"SET statement_timeout='5s'",{});execute(leases.back().get(),"SET lock_timeout='2s'",{});}
}
bool SqlContext::inTransaction() const { return connections.contains(this); }
void SqlContext::appendLegacyEvent(phase11::PostgresOutboxWriter&writer,phase11::OutboxEvent event){requireTransaction();writer.append(connections.at(this),std::move(event));afterCommit([&writer]{writer.committed();});}
void SqlContext::afterCommit(std::function<void()> notification){requireTransaction();notifications[this].push_back(std::move(notification));}
void SqlContext::requireTransaction() const {
    if (!inTransaction()) throw std::logic_error("write requires unit of work");
}
SqlRows SqlContext::query(const std::string &sql, const std::vector<std::string> &parameters) const {
    if (auto it=connections.find(this); it!=connections.end()) return execute(it->second,sql,parameters);
    auto connection=pool_.acquire(); return execute(connection.get(),sql,parameters);
}
void SqlContext::transact(const std::function<void()> &operation) {
    if (inTransaction()) throw std::logic_error("nested unit of work");
    auto lease=pool_.acquire(); auto *connection=lease.get();
    execute(connection,"BEGIN",{}); connections.emplace(this,connection);
    try {
        // 构造时已设置连接级期限，不在每条事件事务中重复发送两次 SET。
        operation(); execute(connection,"COMMIT",{}); connections.erase(this);
    } catch (...) {
        connections.erase(this);
        notifications.erase(this);
        if (auto *result=PQexec(connection,"ROLLBACK")) PQclear(result);
        throw;
    }
    auto pending=notifications.extract(this);
    if(!pending.empty())for(auto&notify:pending.mapped())try{notify();}catch(...){}
}
}
