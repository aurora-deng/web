#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace nebula::client {
using Row=std::vector<std::optional<std::string>>;
/** Windows 系统 SQLite 的小型 RAII 适配器；数据访问总是绑定参数，不拼接用户输入。 */
class SqliteStore final {
public:
    explicit SqliteStore(const std::filesystem::path&database);
    ~SqliteStore();
    SqliteStore(const SqliteStore&)=delete;SqliteStore&operator=(const SqliteStore&)=delete;
    std::vector<Row> query(const std::string&sql,const Row&parameters={});
    void execute(const std::string&sql,const Row&parameters={});
    std::string version()const;
    void transact(const std::function<void()>&operation);
private:struct Impl;std::unique_ptr<Impl>impl_;
};
}
