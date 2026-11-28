#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace webserver::function3 {
using Id = std::uint64_t;
using Milliseconds = std::int64_t;
enum class Role { User, Moderator, Administrator };
enum class Failure { Invalid, Unauthenticated, Forbidden, NotFound, Conflict, Busy };
class BusinessError : public std::runtime_error {
public:
    BusinessError(Failure value, std::string message) : std::runtime_error(std::move(message)), code(value) {}
    Failure code;
};
struct Actor { Id id{}; std::string name; Role role{Role::User}; bool active{true}; bool interactive{true}; };
class IActorDirectory {
public:
    virtual ~IActorDirectory() = default;
    virtual Actor requireActor(Id user) const = 0;
};
/** 同步事务只在业务执行器中运行，整个回调使用同一个数据库连接。 */
class IUnitOfWork {
public:
    virtual ~IUnitOfWork() = default;
    virtual void transact(const std::function<void()> &operation) = 0;
};
struct DomainEvent {
    Id id{};
    std::string type;
    std::string aggregate;
    Id aggregateId{};
    std::uint64_t version{};
    std::map<std::string, std::string> data;
    Milliseconds createdAt{};
};
class IEventWriter {
public:
    virtual ~IEventWriter() = default;
    virtual void appendEvent(DomainEvent event) = 0;
};
inline Actor activeActor(const IActorDirectory &directory, Id user) {
    if (!user) throw BusinessError(Failure::Unauthenticated, "login required");
    auto actor = directory.requireActor(user);
    if (!actor.active) throw BusinessError(Failure::Forbidden, "account disabled");
    return actor;
}
inline void moderator(const Actor &actor) {
    if (actor.role == Role::User) throw BusinessError(Failure::Forbidden, "moderator required");
}
}
