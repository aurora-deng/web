#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace nebula::fleet::sqlite {

class Database {
public:
    Database(const std::filesystem::path& path, std::string_view role, std::string_view owner);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(const char* sql);
    sqlite3* handle() const noexcept { return db_; }

private:
    sqlite3* db_{};
};

class Statement {
public:
    Statement(Database& db, const char* sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement& bind(int index, std::string_view value);
    Statement& bind(int index, std::int64_t value);
    bool next();
    void run();
    std::string string(int col) const;
    std::int64_t integer(int col) const;

private:
    sqlite3_stmt* stmt_{};
};

class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    void operator=(const Transaction&) = delete;
    void commit();

private:
    Database& db_;
    bool committed_{};
};
}
