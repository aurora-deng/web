#include "UserRecoveryBudget.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include <algorithm>
#include <stdexcept>
namespace webserver::phase11 {
UserRecoveryBudget::UserRecoveryBudget(std::size_t capacity,double rate,double burst,std::size_t concurrent):capacity_(capacity),concurrent_(concurrent),rate_(rate),burst_(burst){if(!capacity||!concurrent||rate<=0||burst<1)throw std::invalid_argument("invalid recovery budget");}
UserRecoveryBudget::Lease UserRecoveryBudget::acquire(UserId user,Clock::time_point now){
    std::lock_guard lock(mutex_);
    auto found=users_.find(user);
    if(found==users_.end()){
        if(users_.size()>=capacity_)std::erase_if(users_,[&](const auto&item){return !item.second.active&&now-item.second.updated>std::chrono::minutes(1);});
        if(users_.size()>=capacity_)throw ApplicationError(ErrorCode::RateLimited,"history recovery capacity reached; retry later");
        found=users_.emplace(user,Entry{burst_,now,0}).first;
    }
    auto&e=found->second;e.tokens=std::min(burst_,e.tokens+std::max(0.0,std::chrono::duration<double>(now-e.updated).count())*rate_);e.updated=now;
    if(e.active>=concurrent_||e.tokens<1)throw ApplicationError(ErrorCode::RateLimited,"history recovery budget exceeded; retry with backoff");
    --e.tokens;++e.active;return Lease(this,user);
}
void UserRecoveryBudget::release(UserId user)noexcept{std::lock_guard lock(mutex_);auto it=users_.find(user);if(it!=users_.end()&&it->second.active)--it->second.active;}
}
