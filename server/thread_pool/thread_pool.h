// =============================================================================
// 文件名：thread_pool.h
// 职责：声明固定大小的线程池 ThreadPool——预创建一组 Worker 线程复用，
//       通过条件变量唤醒机制把任务派发给空闲线程执行，避免每个任务都新建/销毁线程。
//
// 【生活比喻】
// 线程池像一家酒店的"厨师班底"：开业时一次招好 N 个厨师（worker 线程），
// 他们常驻后厨等订单（条件变量 wait）。来订单就 push 进任务队列，唤醒一个厨师
// 去领做；做完继续等下一单。打烊时（析构）设 stop 标志，唤醒所有厨师一起收工。
// 比起每来一单就临时招厨师（new thread）、做完就开除（detach/join），效率高得多。
//
// 关键技术点（初学者重点理解）：
// 1. 线程复用：N 个线程常驻循环 [wait→take→run]，省去线程创建/销毁的 syscall 开销。
// 2. mutex + condition_variable 经典生产者-消费者模型：
//    - 生产者（addTask）加锁 push 任务，notify_one 唤醒一个消费者；
//    - 消费者（worker）加锁 wait 任务，被唤醒后 pop 出来执行。
// 3. MAX_THREAD_POOL_QUEUE 限流（背压）：队列满时 addTask 返回 false，
//    让上层（Executor）据此返回 503，防止任务无限堆积撑爆内存。
// 4. stop 标志 + notify_all 实现优雅退出：析构时设 stop=true，唤醒所有 worker，
//    它们检查到 stop && tasks.empty() 后退出循环，析构 join 等所有线程结束。
// =============================================================================
#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include<iostream>
#include<vector>
#include<queue>
#include<thread>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<unistd.h>
#include <utility>

// 背压修复处：线程池任务队列最大长度
// 防止高并发下任务无限堆积导致 OOM；满时 addTask 返回 false 让上层降级
#define MAX_THREAD_POOL_QUEUE 4096

/**
 * @brief 固定大小线程池
 *
 * 【通俗解释】
 * 一群常驻厨师（workers）+ 一个订单队列（tasks）+ 一把锁（mtx）+ 一个叫号铃（cv）。
 * 厨师们循环 [等铃→抢锁→领单→放锁→炒菜]，订单来了按铃通知一个厨师领单。
 */
class ThreadPool{
    private:
    std::vector<std::thread> workers;          // 常驻 Worker 线程数组
    std::queue<std::function<void()>> tasks;   // 待执行任务队列（FIFO）

    std::mutex mtx;                            // 保护 tasks 队列的互斥锁
    std::condition_variable cv; // 用于通知线程有新任务的条件变量
    bool stop=false;                           // 退出标志：true 时通知所有 worker 收工

    void startThreadPool(size_t numThreads) ;
    public:
    /**
     * @brief 构造函数：创建指定数量的 Worker 线程
     * @param threadPoolSize 线程数（建议等于 CPU 核数）
     */
    ThreadPool(int threadPoolSize);

    /**
     * @brief 析构函数：设 stop、唤醒所有线程、join 等待退出
     */
    ~ThreadPool();

    /**
     * @brief 添加任务到队列
     * @param task 可调用对象
     * @return true=成功入队；false=队列满（背压）
     *
     * 【通俗解释】
     * 加锁→检查队列是否满→push 任务→解锁→按铃（notify_one）叫一个厨师来领。
     * 队列满时直接返回 false，让上层走降级路径（如返回 503）。
     */
    bool addTask(std::function<void()> task);        //添加任务到线程池里面去
};


#endif
