// ============================================================
// 文件名：SegmentPool.cpp
// ------------------------------------------------------------
// 【生活比喻：快递分拣中心的托盘回收站】
// SegmentPool 是"托盘回收站"：服务器要发送的响应数据被切成一段段 Segment
// （像一个个快递包裹），装进 Block（托盘，最多 64 段）里。
// 托盘用完不能扔，送回回收站下次接着装，避免反复 new/delete Block。
//
// 【为什么要把数据切成段？】
// 一次 HTTP 响应可能是：状态行 + 多个头 + 一段 body + 文件 mmap 区。
// 这些数据散落在不同内存区（vector 里、mmap 区、常量字符串里），
// 切成 Segment 用指针 + 长度描述，配合 writev 一次性 scatter 发送，
// 既不用把数据拼成一块大内存（省拷贝），又能用一次系统调用发完。
//
// Block 就是装这些 Segment 指针的"托盘"，SegmentPool 管理托盘的借还。
// ============================================================
//
// 关键技术点（初学者重点理解）：
//   1. 段(Segment)+块(Block)两级结构：Block 固定 64 个 Segment 槽，
//      定长数组，栈上可用零 malloc。
//   2. writev 批量发送：BlockToIov 把 Segment 转成 iovec 数组，
//      一次 writev 系统调用发完整个 Block，减少 syscall 次数。
//   3. 托盘复用：acquire/release 配对，Block 用完归还，idx 归零重置。
//   4. 互斥锁保护：多 SubReactor 线程并发借托盘，mtx 串行化访问 freeList。
// ============================================================

#include "SegmentPool.h"

/**
 * @brief 借一个 Block 托盘（领一个空托盘开始装包裹）
 * @return 可用的 Block*，idx 已归零
 *
 * 借托盘逻辑：
 *   1. 回收站有空托盘 → 拿最后一个，idx 归零（清空使用痕迹），返回。
 *   2. 没有空托盘 → new 一个新的 Block。
 *
 * 【为什么 idx=0 而不 memset？】
 * Block 内部是 Segment segs[64] 定长数组，idx 是"用了几个槽"的计数。
 * 归零 idx 就表示"托盘空了"，旧的 segs 内容会被下次写入覆盖，
 * 不用浪费时间 memset 64 个 Segment，O(1) 重置。
 */
Block *SegmentPool::acquire()
{
    std::lock_guard<std::mutex> lock(mtx);
    if(!freeList.empty())
    {
        Block* b=freeList.back();
        freeList.pop_back();
        b->idx=0;   // 托盘清空：使用计数归零，旧 segs 会被下次写入覆盖
        return b;
    }
    return new Block();
}

/**
 * @brief 还一个 Block 托盘（装完送回回收站）
 * @param b 要归还的 Block 指针
 *
 * 还托盘逻辑：加锁、idx 归零、压回 freeList。
 * 注意本实现没有上限保护（与 BufferPoll 不同），调用方需确保归还数量可控。
 */
void SegmentPool::release(Block *b)
{
    std::lock_guard<std::mutex> lock(mtx);
    b->idx=0;       // 清空使用计数，托盘恢复"空"状态
    freeList.push_back(b);
}

/**
 * @brief 获取 SegmentPool 全局单例（进回收站的门）
 * @return 唯一的 SegmentPool 引用
 *
 * 与 BufferPoll::instance 同理，C++11 static 局部变量线程安全懒加载。
 * 整个进程共享一个托盘回收站。
 */
SegmentPool &SegmentPool::instance()
{
    static SegmentPool p;
    return p;
}
