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

// 性能修复处：BlockToIov 改为写入调用方提供的固定数组并返回数量
// 原代码签名为 (const Block*, std::vector<iovec>&)，每次调用都触发 vector 堆分配
// Block 内 segs[64] 是定长数组，最多 64 段，改用栈上 iovec[64] 即可零分配
inline int BlockToIov(const Block *block, iovec *out, int maxCount)
{
    int n = 0;
    for (int i = 0; i < block->idx && n < maxCount; i++)
    {
         const auto &x = block->segs[i];
        if (x.len > 0)
        {
            out[n].iov_base = const_cast<char *>(x.data);
            out[n].iov_len = x.len;
            n++;
        }
    }
    return n;
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