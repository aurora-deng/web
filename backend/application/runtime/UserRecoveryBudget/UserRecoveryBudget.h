#pragma once
#include "backend/application/domain/Domain/Domain.h"
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <utility>
namespace webserver::phase11 {
/** 恢复预算像每用户的取号机：限制历史查询频率和同时在办的任务，不占住 Reactor 等待。 */
class UserRecoveryBudget final {
public:
    using Clock=std::chrono::steady_clock;
    class Lease final {
    public:
        Lease(Lease&&other)noexcept:owner_(std::exchange(other.owner_,nullptr)),user_(other.user_){}
        Lease(const Lease&)=delete;Lease&operator=(const Lease&)=delete;
        ~Lease(){if(owner_)owner_->release(user_);}
    private:
        friend class UserRecoveryBudget;
        Lease(UserRecoveryBudget*owner,UserId user):owner_(owner),user_(user){}
        UserRecoveryBudget*owner_;UserId user_;
    };
    explicit UserRecoveryBudget(std::size_t capacity=4096,double rate=60,double burst=30,std::size_t concurrent=2);
    Lease acquire(UserId user,Clock::time_point now=Clock::now());
private:
    void release(UserId user)noexcept;
    struct Entry{double tokens;Clock::time_point updated;std::size_t active{};};
    std::mutex mutex_;std::unordered_map<UserId,Entry>users_;
    std::size_t capacity_,concurrent_;double rate_,burst_;
};
}
