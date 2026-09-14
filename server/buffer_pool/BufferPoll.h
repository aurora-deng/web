// ============================================================
// 文件名：BufferPoll.h
// ------------------------------------------------------------
// 【生活比喻：共享单车停车棚（棚的图纸）】
// 这是 BufferPoll 的声明文件，定义"停车棚"长什么样：
//   - 一个 mtx_（门锁）：多线程借还车时排队用
//   - 一个 pool（车架）：存放空闲 Buffer 的容器
//   - 三个接口：instance() 进门、acquire() 借车、release() 还车
//
// 实现细节见 BufferPoll.cpp。
// ============================================================
//
// 关键技术点（初学者重点理解）：
//   1. 单例模式：全局唯一实例，所有 SubReactor 线程共享一个停车棚。
//   2. shared_ptr 管理：借出的是 shared_ptr，引用计数自动回收，
//      哪怕调用方忘记 release，对象也不会泄漏，只是没回池复用。
//   3. mutex 串行化：freeList 是共享资源，必须加锁保护。
//   4. 头文件只声明不实现：降低编译依赖，实现改动不触发重编译。
// ============================================================
#ifndef BUFFER_POOL_H
#define BUFFER_POOL_H

#include<vector>
#include <mutex>
#include<memory>

#include"server/Buffer/Buffer.h"


/**
 * @brief Buffer 对象池：共享单车停车棚
 *
 * 【设计动机 通俗解释】
 * 服务器高并发下，Buffer 的 new/delete 是隐形性能杀手。
 * BufferPoll 把用过的 Buffer 攒起来循环用，把"每请求一次 malloc"
 * 降为"启动后几乎零 malloc"，既省 CPU 又防内存碎片。
 */
class BufferPoll
{
private:
    std::mutex mtx_;                              // 门锁：保护 freeList 多线程访问
    std::vector<std::shared_ptr<Buffer>>pool;     // 车架：空闲 Buffer 栈，后进先出
public:
    /// @brief 进停车棚的门：返回全局唯一实例（单例）
    static BufferPoll& instance();
    /// @brief 借一辆车（Buffer），池空则现造一辆
    std::shared_ptr<Buffer> acquire();
    /// @brief 还一辆车回棚，满了（>=1024）就丢弃让其自动析构
    void release(std::shared_ptr<Buffer>);
};



#endif
