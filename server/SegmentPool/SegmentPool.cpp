#include "SegmentPool.h"

Block *SegmentPool::acquire()
{
    std::lock_guard<std::mutex> lock(mtx);
    if(!freeList.empty())
    {
        Block* b=freeList.back();
        freeList.pop_back();
        b->idx=0;
        return b;
    }
    return new Block();
}

void SegmentPool::release(Block *b)
{
    std::lock_guard<std::mutex> lock(mtx);
    b->idx=0;
    freeList.push_back(b);
}

SegmentPool &SegmentPool::instance()
{
    static SegmentPool p;
    return p;
}
