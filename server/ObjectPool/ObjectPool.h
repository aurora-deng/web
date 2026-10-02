// ============================================================
// 文件名：ObjectPool.h
// ------------------------------------------------------------
// 【生活比喻：酒店布草间（通用模板版）】
// ObjectPoll 是一个类模板，把"布草间"做成通用设施——
// 不管是 HttpRequest、HttpResponse 还是别的什么对象，只要 T 提供 reset()，
// 都能套用同一套 acquire/release 逻辑循环复用。
//
// 【模板池 通俗解释】
// 写一遍池化逻辑，T 换成 HttpRequest 就是请求池，换成 HttpResponse 就是响应池。
// C++ 模板在编译期生成具体代码，零运行时开销，比 Java 泛型更高效。
//
// 与 BufferPoll 的区别：
//   BufferPoll 池化的是 shared_ptr<Buffer>（智能指针，引用计数管理生命周期）；
//   ObjectPoll  池化的是 unique_ptr<T>/裸指针 T*（独占所有权，更轻量）。
// ============================================================
//
// 关键技术点（初学者重点理解）：
//   1. 模板复用：一份代码服务多种类型，编译期具现化，零运行时开销。
//   2. unique_ptr 独占：freeList 里存 unique_ptr<T>，归还时转移所有权进池，
//      借出时 release() 释放裸指针给调用方，调用方用完再 release() 回收。
//   3. reset() 契约：要求 T 提供 reset() 把对象恢复到初始状态，
//      保证每次借出去的都是"干净"对象，不残留上次的数据。
//   4. Phase 10 分片：64 个线程分片替代全局单锁；本地为空时可从别的分片窃取，
//      兼顾 Reactor 本地性和跨线程归还。每片最多缓存 64 个对象，避免峰值后无限驻留。
// ============================================================
#ifndef OBJECT_POOL_H
#define OBJECT_POOL_H
#include <array>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>
#include <cstddef>

/**
 * @brief 通用对象池模板：酒店布草间
 * @tparam T 池化对象类型，必须提供 reset() 成员函数
 *
 * 【生命周期 通俗解释】
 *   acquire()：从布草柜领一床干净床单
 *     - 柜里有 → 拿出来 reset() 后交给你（裸指针 T*）
 *     - 柜里空 → new 一个新的给你
 *   release(obj)：用完送回洗涤复用
 *     - obj->reset() 清干净
 *     - 包成 unique_ptr 塞回 freeList
 *
 * 调用方拿到的是裸指针 T*，必须保证用完 release 回收，否则内存泄漏。
 * 这里不用 shared_ptr 是为了省引用计数的原子操作开销，靠调用方纪律保证。
 */
// 使用模版池化函数
template<class T>
class ObjectPoll
{
private:
    static constexpr std::size_t kShardCount = 64;
    static constexpr std::size_t kMaxObjectsPerShard = 64;

    struct alignas(64) Shard
    {
        mutable std::mutex mutex;
        std::vector<std::unique_ptr<T>> freeList;
    };

    static std::size_t localShard() noexcept
    {
        static std::atomic<std::size_t> next{0};
        static thread_local const std::size_t index =
            next.fetch_add(1, std::memory_order_relaxed) % kShardCount;
        return index;
    }

    std::array<Shard, kShardCount> shards_;
    std::atomic<std::size_t> available_{0};
public:
    /**
     * @brief 借一个对象（领一床干净床单）
     * @return 可用的裸指针 T*，调用方负责用完 release()
     *
     * 【为什么返回裸指针而不返回 unique_ptr？】
     * 调用方拿到对象后会长期使用（比如协程跨 co_await 持有），
     * 返回裸指针更灵活，调用方自己管理何时归还，不用受 unique_ptr
     * 移动语义的限制。
     */
    T* acquire()
    {
        const auto preferred = localShard();
        // 通常第一个分片就命中；只有 Worker 借、Reactor 还这类跨线程交接时
        // 才向其他分片“借一件”，避免对象永久堆在某一线程名下。
        for (std::size_t offset = 0; offset < kShardCount; ++offset)
        {
            auto &shard = shards_[(preferred + offset) % kShardCount];
            std::unique_ptr<T> object;
            {
                std::lock_guard lock(shard.mutex);
                if (shard.freeList.empty())
                    continue;
                object = std::move(shard.freeList.back());
                shard.freeList.pop_back();
            }
            available_.fetch_sub(1, std::memory_order_relaxed);
            object->reset();
            return object.release();
        }
        return new T();
    }
    /**
     * @brief 归还对象（送回洗涤复用）
     * @param obj 用完的对象指针，传入会被接管所有权
     *
     * 先 reset 再放入当前线程分片：保证对象干净，也避免所有 Reactor 争同一把锁。
     */
    void release(T* obj)
    {
        if (!obj)
            return;
        std::unique_ptr<T> object(obj);
        object->reset();
        auto &shard = shards_[localShard()];
        std::lock_guard lock(shard.mutex);
        if (shard.freeList.size() >= kMaxObjectsPerShard)
            return;
        shard.freeList.push_back(std::move(object));
        available_.fetch_add(1, std::memory_order_relaxed);
    }

    size_t available() const
    {
        return available_.load(std::memory_order_relaxed);
    }
};



#endif
