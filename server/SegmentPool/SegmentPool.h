// ============================================================
// 文件名：SegmentPool.h
// ------------------------------------------------------------
// 【生活比喻：快递分拣中心（托盘与包裹的图纸）】
// 本文件定义响应数据的"切片发送"数据结构：
//   - Segment     ：一个包裹（一段数据，用指针+长度+类型描述）
//   - Block       ：一个托盘（固定装 64 个包裹）
//   - SegmentPool ：托盘回收站（管理 Block 的借还）
//   - BlockGuard  ：托盘的"自动归还套"（RAII，作用域结束自动还）
//
// 【为什么要切片？】
// HTTP 响应由多部分拼成（状态行、头、body、文件），它们在不同内存区。
// 切成 Segment 用指针引用，配合 writev 一次系统调用批量发送，
// 实现"零拷贝"——不把数据搬到一个大缓冲里再发，而是直接告诉内核
// "这几块内存按顺序发出去"。既省内存拷贝，又省 syscall 次数。
// ============================================================
//
// 关键技术点（初学者重点理解）：
//   1. Segment 类型枚举：MEM/MMAP/BUFFER/CONST 区分数据来源，
//      为后续不同来源的内存管理（mmap 解映射、Buffer 归还池）铺路。
//   2. Block 定长数组 segs[64]：避免 vector 动态分配，栈上可用，零 malloc。
//   3. BlockToIov 写入栈数组：性能修复点，避免每次调用 vector 堆分配。
//   4. BlockGuard RAII：构造借托盘、析构还托盘，异常安全，防忘还泄漏。
// ============================================================
#ifndef SEGMENT_POOL_H
#define SEGMENT_POOL_H
#include <cstddef>
#include <vector>
#include <cstring>
#include <mutex>
#include <sys/uio.h>

// 引入segmentview模型，统一发送信息内存，
// 为后续内存池做铺垫,彻底落实零拷贝的内存指向,抽象类型发送，有利于可扩展性

/**
 * @brief 数据段：响应数据的一个"包裹"
 *
 * 【Segment 通俗解释】
 * 一个 Segment 不持有数据，只"指着"某块内存说：
 * "从 data 开始、长度 len 的这块，按 type 类型处理，发完可能要归还 owner"。
 * 这样响应数据不用集中拷贝，原地引用即可发送。
 *
 * type 区分四种数据来源，决定发送后如何处理底层内存：
 */
struct Segment
{
    const char *data = nullptr;   // 包裹地址：指向真实数据
    size_t len = 0;               // 包裹大小
    enum Type
    {
        MEM,     // 普通堆内存，发送后可能需 free
        MMAP,    // mmap 映射区（如大文件），发送后需 munmap
        BUFFER,  // 来自 Buffer 池，发送后归还 BufferPoll
        CONST    // 常量字符串（如状态行），无需任何处理
    } type = CONST;

    void *owner = nullptr; // 可选：用于归还池（如 Buffer 指针、Block 指针）
};


/**
 * @brief 数据块：装 64 个 Segment 的"托盘"
 *
 * 【Block 通俗解释】
 * 一个托盘最多装 64 个包裹（Segment）。idx 是"已装了几个"的计数。
 * 定长数组 segs[64] 而非 vector，保证 Block 本身大小固定，
 * 可放栈上、可池化复用，零动态分配。
 */
struct Block
{
    Segment segs[64];  // 包裹槽：定长 64，编译期确定大小
    int idx = 0;       // 已用槽位数：下次 segs[idx] 是下一个空位
};

// 性能修复处：BlockToIov 改为写入调用方提供的固定数组并返回数量
// 原代码签名为 (const Block*, std::vector<iovec>&)，每次调用都触发 vector 堆分配
// Block 内 segs[64] 是定长数组，最多 64 段，改用栈上 iovec[64] 即可零分配

/**
 * @brief 把 Block 里的 Segment 转成 writev 需要的 iovec 数组
 * @param block    源数据块
 * @param out      调用方提供的 iovec 输出数组（建议 64 长度）
 * @param maxCount out 数组最大容量
 * @return 实际填入的 iovec 数量
 *
 * 【writev 通俗解释】
 * writev 是 Linux 的"批量写"系统调用：传一组 iovec（每项是{指针,长度}），
 * 内核按顺序把这些内存内容一次写到 fd，等价于多次 write 但只一次 syscall。
 * 把多个 Segment 打包成 iovec 一次性发，大幅减少用户态/内核态切换开销。
 *
 * 【为什么写入调用方数组而不是返回 vector？】
 * 返回 vector 会触发堆分配（malloc），高并发下成为瓶颈。
 * 让调用方在栈上准备 iovec[64]，本函数填进去返回数量，全程零 malloc。
 */
inline int BlockToIov(const Block *block, iovec *out, int maxCount)
{
    int n = 0;
    for (int i = 0; i < block->idx && n < maxCount; i++)
    {
         const auto &x = block->segs[i];
        if (x.len > 0)   // 跳过空段，避免 writev 传 0 长度触发 EINVAL
        {
            out[n].iov_base = const_cast<char *>(x.data);
            out[n].iov_len = x.len;
            n++;
        }
    }
    return n;
}

/**
 * @brief 内存段池：托盘回收站
 *
 * 【SegmentPool 通俗解释】
 * Block（托盘）用完送回这里，下次 acquire 直接领旧的不 new 新的。
 * 与 BufferPoll、ObjectPool 同理，都是"循环复用省 malloc"。
 * freeList 用裸指针 Block*，靠调用方/BlockGuard 保证配对归还。
 */
class SegmentPool
{
public:
    std::vector<Block *> freeList;  // 回收站：空闲托盘栈
    std::mutex mtx;                 // 门锁：保护 freeList 多线程访问
    /// @brief 借一个空托盘，池空则 new 一个
    Block *acquire();
    /// @brief 还一个托盘，idx 归零后压回栈
    void release(Block *b);
    /// @brief 进回收站的门：全局单例
    static SegmentPool &instance();
};


/**
 * @brief 托盘自动归还套（RAII 守卫）
 *
 * 【RAII 通俗解释】
 * BlockGuard 构造时借一个托盘，析构时自动还——不管函数怎么退出
 * （正常 return、提前 return、异常抛出），托盘都不会漏还。
 * 这就是 C++ 的 RAII：把资源生命周期绑定到对象生命周期，异常安全。
 *
 * 用法：函数里写 BlockGuard guard{pool, pool.acquire()};
 * 函数结束自动 release，不用手动记得还，防忘防漏。
 */
struct BlockGuard
{
    SegmentPool& pool;
    Block* block;
    ~BlockGuard()
    {
        if(block)
            pool.release(block);  // 析构自动归还托盘，异常安全
    }

};
#endif
