// ============================================================
// 文件名：BufferPoll.cpp
// ------------------------------------------------------------
// 【生活比喻：共享单车停车棚】
// BufferPoll 是一个"共享单车停车棚"：预先停一批 Buffer（共享单车），
// 谁要用车就来 acquire() 骑走，用完 release() 还回棚里，下次别人接着骑。
// 绝不在用完就把车砸了（delete），也不每次用车都现造一辆（new）。
//
// 为什么要这样？
//   服务器每秒要处理成千上万个请求，每个请求都要一个 Buffer 接数据。
//   如果每次 new Buffer、用完 delete，会让 malloc/free 疯狂运转，
//   产生内存碎片、降低性能。停车棚让同一批 Buffer 反复流转，
//   malloc 次数从"每请求一次"降到"几乎不再增长"。
// ============================================================
//
// 关键技术点（初学者重点理解）：
//   1. 单例模式：全局唯一停车棚，instance() 返回 static 局部变量，
//      线程安全且懒加载，所有 SubReactor 共享。
//   2. 互斥锁保护：多线程同时借/还车，用 std::mutex 串行化访问 freeList。
//   3. 上限保护：停车棚最多停 1024 辆，超了就真 delete，防止内存无限增长。
//   4. 复用前清理：acquire 时 retrieve 清掉残留数据，release 时 clear 归零，
//      保证借出去的车是"干净的"。
// ============================================================

#include "BufferPoll.h"

/**
 * @brief 获取 BufferPoll 全局单例（进停车棚的门）
 * @return 唯一的 BufferPoll 引用
 *
 * 【单例 通俗解释】
 * 整个进程只有一个停车棚。C++11 起，static 局部变量的初始化是
 * 线程安全的（编译器自动加锁），所以这种写法既懒加载又安全，
 * 不用手动写双重检查锁。
 */
BufferPoll &BufferPoll::instance()
{
    static BufferPoll bufferpool;
    return bufferpool;
}

/**
 * @brief 从池里借一个 Buffer（从棚里骑走一辆车）
 * @return 可用的 shared_ptr<Buffer>
 *
 * 借车逻辑：
 *   1. 棚里有车 —— 拿最后一辆，pop 出栈，清掉残留可读数据后返回。
 *   2. 棚里没车 —— 现造一辆新的（make_shared），第一次难免要 malloc，
 *      但后续就靠复用省下了。
 *
 * 【为什么 retrieve 而不 clear？】
 * retrieve(readableBytes()) 把读指针推到写指针位置，逻辑上清空，
 * 但保留 vector 容量，下次还能直接装那么多，不用再扩容。
 * 两者效果类似，这里用 retrieve 表达"消费掉所有可读数据"的语义。
 */
std::shared_ptr<Buffer> BufferPoll::acquire()
{
    std::lock_guard lock(mtx_);

    if (!pool.empty())
    {
        auto p = pool.back();
        pool.pop_back();
        p->retrieve(p->readableBytes());  // 清掉残留可读数据，车擦干净再借出
        return p;
    }
    return std::make_shared<Buffer>();
}

/**
 * @brief 把 Buffer 还回池里（把车停回棚里）
 * @param p 要归还的 Buffer，传 nullptr 直接忽略
 *
 * 还车逻辑：
 *   1. 空指针直接返回，防呆。
 *   2. clear() 归零指针，让车"干净"。
 *   3. 加锁后塞回 freeList 尾部。
 *   4. 棚里已满（>=1024）就不收了，shared_ptr 引用计数归零自动 delete。
 *
 * 【上限 1024 的意义】
 * 防止突发流量下借出去的车全还回来、棚无限膨胀吃光内存。
 * 1024 个 8KB Buffer 约 8MB，是个安全的内存预算上限。
 */
void BufferPoll::release(std::shared_ptr<Buffer> p)
{
    if(!p)return;
    p->clear();
    std::lock_guard lock(mtx_);

    if (pool.size() < 1024)
    {
        pool.push_back(std::move(p));
    }
}
