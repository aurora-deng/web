#include "SqliteDatabase.h"

#include "../../domain/FleetTypes/FleetTypes.h"

#include <bit>
#ifdef _WIN32
#include <windows.h>
#else
#include <sqlite3.h>
#endif

namespace nebula::fleet::sqlite {
namespace {
using Destroy = void (*)(void*);
using Callback = int (*)(void*, int, char**, char**);

#ifdef _WIN32
// Reuse the Windows SQLite runtime without picking up a DLL beside the executable.
using Int64 = long long;
class Api {
public:
    Api() : module_(LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!module_) throw Error(ErrorCode::storage, "Windows SQLite is unavailable");
        try {
            load(open, "sqlite3_open_v2");
            load(close, "sqlite3_close_v2");
            load(timeout, "sqlite3_busy_timeout");
            load(exec, "sqlite3_exec");
            load(prepare, "sqlite3_prepare_v2");
            load(finalize, "sqlite3_finalize");
            load(text, "sqlite3_bind_text");
            load(number, "sqlite3_bind_int64");
            load(step, "sqlite3_step");
            load(columnText, "sqlite3_column_text");
            load(columnInt, "sqlite3_column_int64");
            load(columnBytes, "sqlite3_column_bytes");
        } catch (...) {
            FreeLibrary(module_);
            throw;
        }
    }

    ~Api() { FreeLibrary(module_); }

    int (*open)(const char*, sqlite3**, int, const char*);
    int (*close)(sqlite3*);
    int (*timeout)(sqlite3*, int);
    int (*exec)(sqlite3*, const char*, Callback, void*, char**);
    int (*prepare)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
    int (*finalize)(sqlite3_stmt*);
    int (*text)(sqlite3_stmt*, int, const char*, int, Destroy);
    int (*number)(sqlite3_stmt*, int, Int64);
    int (*step)(sqlite3_stmt*);
    const unsigned char* (*columnText)(sqlite3_stmt*, int);
    Int64 (*columnInt)(sqlite3_stmt*, int);
    int (*columnBytes)(sqlite3_stmt*, int);

private:
    template<class Function>
    void load(Function& function, const char* name) {
        const auto address = GetProcAddress(module_, name);
        if (!address) throw Error(ErrorCode::storage, "Windows SQLite API is incomplete");
        function = std::bit_cast<Function>(address);
    }

    HMODULE module_;
};
constexpr int ok = 0;
constexpr int row = 100;
constexpr int done = 101;
constexpr int openReadWriteCreate = 0x2 | 0x4;
#else
struct Api {
    static constexpr auto open = sqlite3_open_v2;
    static constexpr auto close = sqlite3_close_v2;
    static constexpr auto timeout = sqlite3_busy_timeout;
    static constexpr auto exec = sqlite3_exec;
    static constexpr auto prepare = sqlite3_prepare_v2;
    static constexpr auto finalize = sqlite3_finalize;
    static constexpr auto text = sqlite3_bind_text;
    static constexpr auto number = sqlite3_bind_int64;
    static constexpr auto step = sqlite3_step;
    static constexpr auto columnText = sqlite3_column_text;
    static constexpr auto columnInt = sqlite3_column_int64;
    static constexpr auto columnBytes = sqlite3_column_bytes;
};
constexpr int ok = SQLITE_OK;
constexpr int row = SQLITE_ROW;
constexpr int done = SQLITE_DONE;
constexpr int openReadWriteCreate = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
#endif

const Api& api() {
    static const Api instance;
    return instance;
}

void checked(int status, const char* operation) {
    if (status != ok) throw Error(ErrorCode::storage, operation);
}
}

Database::Database(const std::filesystem::path& path, std::string_view role, std::string_view owner) {
    const auto utf8 = path.u8string();
    if (utf8.empty()) throw Error(ErrorCode::invalid, "ledger path is empty");
    try {
        checked(api().open(reinterpret_cast<const char*>(utf8.c_str()), &db_, openReadWriteCreate, nullptr),
                "cannot open fleet ledger");
        checked(api().timeout(db_, 500), "cannot configure SQLite timeout");
        // One authority per private file. In rollback journal mode the first
        // exclusive write keeps the lock until this process closes the ledger.
        exec("PRAGMA locking_mode=EXCLUSIVE");
        {
            Transaction setup(*this);
            Statement fields(*this, "PRAGMA user_version");
            if (!fields.next() || (fields.integer(0) != 0 && fields.integer(0) != 1)) {
                throw Error(ErrorCode::storage, "unsupported fleet ledger schema");
            }
            const auto version = fields.integer(0);
            if (!version) {
                Statement existing(*this, "SELECT name FROM sqlite_master WHERE type='table' LIMIT 1");
                if (existing.next()) throw Error(ErrorCode::conflict, "path refers to an existing database");
                exec("CREATE TABLE fleet_identity (role TEXT NOT NULL, owner TEXT NOT NULL)");
                Statement(*this, "INSERT INTO fleet_identity VALUES (?,?)").bind(1, role).bind(2, owner).run();
                exec("PRAGMA user_version=1");
            } else {
                Statement identity(*this, "SELECT role, owner FROM fleet_identity");
                if (!identity.next() || identity.string(0) != role || identity.string(1) != owner) {
                    throw Error(ErrorCode::conflict, "fleet ledger role or authority mismatch");
                }
            }
            setup.commit();
        }
        exec("PRAGMA journal_mode=DELETE");
        exec("PRAGMA synchronous=FULL");
        // A version-1 ledger can be opened only by its designated process.
        Transaction lock(*this);
        exec("UPDATE fleet_identity SET role=role");
        lock.commit();
    } catch (...) {
        if (db_) api().close(db_);
        throw;
    }
}

Database::~Database() { api().close(db_); }

void Database::exec(const char* sql) {
    checked(api().exec(db_, sql, nullptr, nullptr, nullptr), "SQLite ledger operation failed");
}

Statement::Statement(Database& db, const char* sql) {
    checked(api().prepare(db.handle(), sql, -1, &stmt_, nullptr), "cannot prepare fleet ledger statement");
}

Statement::~Statement() { api().finalize(stmt_); }

Statement& Statement::bind(int index, std::string_view value) {
    const auto copy = reinterpret_cast<Destroy>(static_cast<std::intptr_t>(-1)); // SQLITE_TRANSIENT
    checked(api().text(stmt_, index, value.empty() ? "" : value.data(), static_cast<int>(value.size()), copy),
            "cannot bind fleet text");
    return *this;
}

Statement& Statement::bind(int index, std::int64_t value) {
    checked(api().number(stmt_, index, value), "cannot bind fleet integer");
    return *this;
}

bool Statement::next() {
    const auto result = api().step(stmt_);
    if (result == row) return true;
    if (result == done) return false;
    throw Error(ErrorCode::storage, "SQLite ledger statement failed");
}

void Statement::run() {
    if (next()) throw Error(ErrorCode::storage, "unexpected SQLite ledger result");
}

std::string Statement::string(int col) const {
    const auto* value = api().columnText(stmt_, col);
    return value ? std::string(reinterpret_cast<const char*>(value), api().columnBytes(stmt_, col)) : std::string{};
}

std::int64_t Statement::integer(int col) const { return api().columnInt(stmt_, col); }

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN EXCLUSIVE"); }
Transaction::~Transaction() {
    if (!committed_) {
        api().exec(db_.handle(), "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

void Transaction::commit() {
    db_.exec("COMMIT");
    committed_ = true;
}
}
