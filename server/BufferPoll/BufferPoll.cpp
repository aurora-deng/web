#include "BufferPoll.h"

BufferPoll &BufferPoll::instance()
{
    static BufferPoll bufferpool;
    return bufferpool;
}

std::shared_ptr<Buffer> BufferPoll::acquire()
{
    std::lock_guard lock(mtx_);

    if (!pool.empty())
    {
        auto p = pool.back();
        pool.pop_back();
        p->retrieve(p->readableBytes());
        return p;
    }
    return std::make_shared<Buffer>();
}

void BufferPoll::release(std::shared_ptr<Buffer> p)
{
    std::lock_guard lock(mtx_);

    if (pool.size() < 1024)
    {
        pool.push_back(std::move(p));
    }
}
