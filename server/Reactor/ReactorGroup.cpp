#include "ReactorGroup.h"

#include "server/Executor/Executor.h"
#include "server/SubReactor/SubReactor.h"
#include "server/http/HttpCodec/HttpCodec.h"

#include <stdexcept>

ReactorGroup::ReactorGroup(HttpCodec &codec, Executor &executor) : codec_(codec), executor_(executor)
{

}

ReactorGroup::~ReactorGroup()
{
    stop();
    join();
}

void ReactorGroup::start(size_t count)
{
     if (count == 0 || !reactors_.empty())
        throw std::logic_error("ReactorGroup requires a non-zero, one-time start");
    // 重置数组大小，更新Reactor数量
    reactors_.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        // 创建Reactor并完成初始化和启动
        reactors_.push_back(std::make_unique<SubReactor>(codec_, executor_));
        reactors_.back()->run();
    }
}

void ReactorGroup::dispatch(int fd)
{
    if (reactors_.empty())
        throw std::logic_error("cannot dispatch before ReactorGroup::start");
    
    reactors_[next_]->addFd(fd);
    next_ = (next_ + 1) % reactors_.size();
}


void ReactorGroup::stop()
{
    for (auto &reactor : reactors_)
        reactor->stop();
}



void ReactorGroup::join()
{
    for (auto &reactor : reactors_)
        reactor->join();
}
