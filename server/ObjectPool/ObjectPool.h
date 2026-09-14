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
//   4. 无上限保护：注意本实现 freeList 没有容量上限，调用方需自行控制归还频率，
//      否则突发流量下池可能无限增长（与 BufferPoll 的 1024 上限不同）。
// ============================================================
#ifndef OBJECT_POOL_H
#define OBJECT_POOL_H
#include<mutex>
#include<memory>
#include<vector>
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
    mutable std::mutex mtx;                    // 门锁：保护 freeList 多线程访问
    std::vector<std::unique_ptr<T>> freeList;  // 布草柜：空闲对象栈，后进先出
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
    T* acquire(){
        std::lock_guard lock(mtx);
        if(freeList.empty())
        {
            // 柜里空了，现造一个新的。冷启动或高并发突发时会走到这里。
            return new T();
        }
        auto p=
        std::move(
            freeList.back()
        );

        freeList.pop_back();
        p->reset();         // 洗干净：恢复初始状态，清掉上次残留数据
        return p.release(); // 释放裸指针所有权交给调用方
    }
    /**
     * @brief 归还对象（送回洗涤复用）
     * @param obj 用完的对象指针，传入会被接管所有权
     *
     * 先 reset 再入栈：保证柜里所有对象都是干净的，下次借出即用。
     */
    void release(T* obj){
        obj->reset();    // 先洗干净，再放回柜，保证柜里都是干净的
        std::lock_guard lock(mtx);
        freeList.emplace_back(obj);  // 包成 unique_ptr 入栈，所有权转移给池
    }

    size_t available() const
    {
        std::lock_guard lock(mtx);
        return freeList.size();
    }
};



#endif
