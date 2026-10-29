#include "SqliteStore.h"
#include <windows.h>
#include <mutex>
#include <stdexcept>
#include <limits>
namespace nebula::client {
namespace {
struct sqlite3;struct sqlite3_stmt;
template<class F>F symbol(HMODULE library,const char*name){const auto value=GetProcAddress(library,name);if(!value)throw std::runtime_error("Windows SQLite API unavailable");return reinterpret_cast<F>(value);}
}
struct SqliteStore::Impl {
    HMODULE library{};sqlite3*database{};std::recursive_mutex mutex;unsigned transactionDepth{};
    int(*open)(const char*,sqlite3**,int,const char*){};
    int(*close)(sqlite3*){};int(*prepare)(sqlite3*,const char*,int,sqlite3_stmt**,const char**){};
    int(*step)(sqlite3_stmt*){};int(*finalize)(sqlite3_stmt*){};
    int(*bindText)(sqlite3_stmt*,int,const char*,int,void(*)(void*)){};int(*bindNull)(sqlite3_stmt*,int){};
    int(*columnCount)(sqlite3_stmt*){};int(*columnType)(sqlite3_stmt*,int){};
    const unsigned char*(*columnText)(sqlite3_stmt*,int){};int(*columnBytes)(sqlite3_stmt*,int){};
    int(*busyTimeout)(sqlite3*,int){};int(*limit)(sqlite3*,int,int){};const char*(*libVersion)(){};
    ~Impl(){if(database&&close)close(database);if(library)FreeLibrary(library);}
};
SqliteStore::SqliteStore(const std::filesystem::path&path):impl_(std::make_unique<Impl>()) {
    auto&i=*impl_;i.library=LoadLibraryExW(L"winsqlite3.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!i.library)throw std::runtime_error("Windows built-in SQLite is unavailable; no fallback download performed");
#define SQL(name,member) i.member=symbol<decltype(i.member)>(i.library,name)
    SQL("sqlite3_open_v2",open);SQL("sqlite3_close_v2",close);SQL("sqlite3_prepare_v2",prepare);
    SQL("sqlite3_step",step);SQL("sqlite3_finalize",finalize);SQL("sqlite3_bind_text",bindText);SQL("sqlite3_bind_null",bindNull);
    SQL("sqlite3_column_count",columnCount);SQL("sqlite3_column_type",columnType);SQL("sqlite3_column_text",columnText);SQL("sqlite3_column_bytes",columnBytes);
    SQL("sqlite3_busy_timeout",busyTimeout);SQL("sqlite3_limit",limit);SQL("sqlite3_libversion",libVersion);
#undef SQL
    const auto encoded=path.u8string();const std::string utf8(encoded.begin(),encoded.end());
    if(i.open(utf8.c_str(),&i.database,0x2|0x4|0x10000,nullptr)!=0)throw std::runtime_error("Cannot open private SQLite database");
    i.busyTimeout(i.database,1000);i.limit(i.database,0,2*1024*1024);i.limit(i.database,2,128);
    execute("PRAGMA foreign_keys=ON");execute("PRAGMA journal_mode=WAL");execute("PRAGMA synchronous=FULL");
    execute("PRAGMA secure_delete=ON");execute("PRAGMA cache_size=-2048");execute("PRAGMA mmap_size=0");
    execute("PRAGMA temp_store=MEMORY");
}
SqliteStore::~SqliteStore()=default;
std::vector<Row> SqliteStore::query(const std::string&sql,const Row&parameters) {
    auto&i=*impl_;std::lock_guard lock(i.mutex);sqlite3_stmt*statement=nullptr;
    if(sql.size()>65536||parameters.size()>128)throw std::runtime_error("Local SQL request exceeds bound");
    if(i.prepare(i.database,sql.c_str(),static_cast<int>(sql.size()),&statement,nullptr)!=0)throw std::runtime_error("Local database statement failed");
    struct Guard{Impl&i;sqlite3_stmt*s;~Guard(){i.finalize(s);}}guard{i,statement};
    for(std::size_t index=0;index<parameters.size();index++) {
        const auto&value=parameters[index];int code;
        if(value){if(value->size()>2*1024*1024)throw std::runtime_error("Local value exceeds bound");
            code=i.bindText(statement,static_cast<int>(index+1),value->data(),static_cast<int>(value->size()),reinterpret_cast<void(*)(void*)>(static_cast<intptr_t>(-1)));}
        else code=i.bindNull(statement,static_cast<int>(index+1));
        if(code!=0)throw std::runtime_error("Local parameter binding failed");
    }
    std::vector<Row> result;
    for(int status;(status=i.step(statement))!=101;) {
        if(status!=100)throw std::runtime_error("Local database operation failed");
        if(result.size()>=1000)throw std::runtime_error("Local result exceeds page bound");
        Row row;for(int column=0;column<i.columnCount(statement);column++) {
            if(i.columnType(statement,column)==5)row.emplace_back(std::nullopt);
            else{const auto*data=i.columnText(statement,column);row.emplace_back(std::string(reinterpret_cast<const char*>(data),i.columnBytes(statement,column)));}
        }result.push_back(std::move(row));
    }return result;
}
void SqliteStore::execute(const std::string&sql,const Row&parameters){(void)query(sql,parameters);}
std::string SqliteStore::version()const{return impl_->libVersion();}
void SqliteStore::transact(const std::function<void()>&operation){
    std::lock_guard lock(impl_->mutex);auto&depth=impl_->transactionDepth;if(depth>=8)throw std::runtime_error("本机事务嵌套过深");
    const bool outer=depth==0;const auto savepoint="local_nested_"+std::to_string(depth);
    execute(outer?"BEGIN IMMEDIATE":"SAVEPOINT "+savepoint);++depth;
    struct Depth{unsigned&value;~Depth(){--value;}}guard{depth};
    try{operation();execute(outer?"COMMIT":"RELEASE "+savepoint);}
    catch(...){try{if(outer)execute("ROLLBACK");else{execute("ROLLBACK TO "+savepoint);execute("RELEASE "+savepoint);}}catch(...){}throw;}
}
}
