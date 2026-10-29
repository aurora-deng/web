#include "backend/application/runtime/UserRecoveryBudget/UserRecoveryBudget.h"
#include "backend/application/business/ApplicationError/ApplicationError.h"
#include <iostream>
using namespace webserver::phase11;
template<class F>void rejected(F f){try{f();}catch(const ApplicationError&e){if(e.code()==ErrorCode::RateLimited)return;throw;}throw std::runtime_error("budget should reject");}
int main(){try{
    UserRecoveryBudget budget(2,2,2,1);const auto now=UserRecoveryBudget::Clock::now();
    {auto one=budget.acquire(1,now);rejected([&]{budget.acquire(1,now);});}
    {auto two=budget.acquire(1,now);}
    rejected([&]{budget.acquire(1,now);});
    {auto refilled=budget.acquire(1,now+std::chrono::seconds(1));auto other=budget.acquire(2,now);rejected([&]{budget.acquire(3,now);});}
    {auto expired=budget.acquire(3,now+std::chrono::minutes(2));}
    std::cout<<"Per-user recovery: concurrency, rate, independent users, bounded entries and expiry passed\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
