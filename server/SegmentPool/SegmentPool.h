#ifndef SEGMENT_POOL_H
#define SEGMENT_POOL_H
#include <cstddef>
#include <vector>
#include <cstring>
#include <mutex>
#include <bits/types/struct_iovec.h>

// 引入segmentview模型，统一发送信息内存，
// 为后续内存池做铺垫,彻底落实零拷贝的内存指向,抽象类型发送，有利于可扩展性
struct Segment
{
    const char *data = nullptr;
    size_t len = 0;
    enum Type
    {
        MEM,
        MMAP,
        BUFFER,
        CONST
    } type = CONST;

    void *owner = nullptr; // 可选：用于归还池
};


struct Block
{
    Segment segs[64];
    int idx = 0;
};

inline void BlockToIov(const Block* block, std::vector<iovec> &out)
{
    out.clear();
    for (auto &x : block->segs)
    {
        out.push_back({(void *)x.data, x.len});
    }
}

class SegmentPool
{
public:
    std::vector<Block *> freeList;
    std::mutex mtx;
    Block *acquire();
    void release(Block *b);
    static SegmentPool &instance();
};


struct BlockGuard
{
    SegmentPool& pool;
    Block* block;
    ~BlockGuard()
    {
        if(block)
            pool.release(block);
    }

};
#endif