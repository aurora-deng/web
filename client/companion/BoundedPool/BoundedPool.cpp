#include "BoundedPool.h"
#include <stdexcept>
namespace nebula::client {
BoundedPool::BoundedPool(std::size_t count,std::size_t capacity):capacity_(capacity){
    if(!count||count>16||capacity<count||capacity>128)throw std::invalid_argument("invalid local worker limits");
    for(std::size_t i=0;i<count;i++)workers_.emplace_back([this]{for(;;){std::function<void()>task;{
        std::unique_lock lock(mutex_);changed_.wait(lock,[this]{return stopping_||!queue_.empty();});if(stopping_&&queue_.empty())return;task=std::move(queue_.front());queue_.pop_front();}
        try{task();}catch(...){} {std::lock_guard lock(mutex_);--pending_;}
    }});
}
BoundedPool::~BoundedPool(){shutdown();}
bool BoundedPool::submit(std::function<void()>task){std::lock_guard lock(mutex_);if(stopping_||pending_>=capacity_)return false;++pending_;queue_.push_back(std::move(task));changed_.notify_one();return true;}
void BoundedPool::shutdown(){
    {std::lock_guard lock(mutex_);if(stopping_)return;stopping_=true;pending_-=queue_.size();queue_.clear();}changed_.notify_all();
    for(auto&thread:workers_)if(thread.joinable())thread.join();
}
}
