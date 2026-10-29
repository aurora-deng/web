// =============================================================================
// 文件名：thread_pool.cpp
// 职责：实现线程池的核心逻辑——启动 Worker 线程、消费任务队列、优雅退出
//
// 【生活比喻】
// 本文件是"厨师班底"的日常工作手册：开店时招 N 个厨师上岗（startThreadPool），
// 每个厨师循环 [等铃→领单→炒菜→再等]；新订单来了 push 进队列按铃（addTask）；
// 打烊时设 stop=true 摇铃叫所有人收工（shutdown/析构）。所有厨师共享一个订单队列，
// 通过一把锁（mtx）和一只铃铛（cv）协调谁领哪一单。
//
// 关键技术点（初学者重点理解）：
// 1. condition_variable::wait 的"谓词"形式：cv.wait(lock, pred) 在 pred 为 false 时
//    释放锁阻塞等待，被 notify 后重新加锁检查 pred，true 才返回。这避免"假唤醒"
//    导致空队列 pop 的 bug。
// 2. 退出条件 stop || !tasks.empty()：①stop=true 且队列空→收工；
//    ②stop=true 但队列还有单→继续把剩下的单做完再收工（优雅退出，不丢任务）。
// 3. task() 在锁外执行：把任务执行移出临界区，让其他厨师能同时领单，
//    否则整个池子退化为串行执行，多线程优势全无。
// 4. addTask 的背压检查：队列满直接返回 false，避免 OOM。这是上层 Executor
//    返回 503 的依据。
// =============================================================================
#include"thread_pool.h"


/**
 * @brief 构造函数：初始化 stop 标志并启动指定数量的 Worker 线程
 * @param threadPoolSize 线程数量
 *
 * 【通俗解释】开店招人：先在初始化列表里设 stop=false（还在营业），
 *            然后调 startThreadPool 招 threadPoolSize 个厨师上岗。
 */
ThreadPool::ThreadPool(int threadPoolSize):stop(false)
{
    startThreadPool(threadPoolSize);
}

/**
 * @brief 启动线程池：创建 numThreads 个 Worker 线程，每个线程跑消费循环
 * @param numThreads 线程数量
 *
 * 【通俗解释】
 * 循环招 N 个厨师，每个厨师入职后干同样的活：
 *   while(true) {
 *     等铃响（cv.wait，加锁检查 stop||!empty）
 *     如果打烊且没单了 → 下班 return
 *     抢锁→领单（pop）→放锁
 *     炒菜（task()）  ← 锁外执行，让别的厨师也能领单
 *   }
 */
void ThreadPool::startThreadPool(size_t numThreads)
{
    // 循环创建线程
    for (size_t i = 0; i < numThreads; ++i)
    {
        // 创建线程并放到对应容器
        workers.emplace_back([this]
        {
            while (true)
            {
                std::function<void()>task;  //创建任务

                {
                    // ----- 临界区开始：抢锁领单 -----
                    std::unique_lock<std::mutex> lock(this->mtx);               //加锁
                    this->cv.wait(lock,[this]{                                  //等待条件变量通知
                        return this->stop || !this->tasks.empty();
                });
                    // wait 返回时已重新加锁：要么 stop=true，要么队列有单
                    // 若打烊且队列已空→下班；若打烊但还有单→继续做（优雅退出不丢任务）

                if(this->stop&&tasks.empty()) return;            //当线程池停止或者任务队列为空，退出线程


                task=std::move(tasks.front());          //将任务取出
                tasks.pop();                            //移除消息队列
                // ----- 临界区结束：lock 析构放锁 -----
            }
                task();         //执行队伍（锁外执行，让其他线程能并发领单）

            }});
    }
}


/**
 * @brief 析构函数：优雅关闭线程池
 *
 * 【通俗解释】
 * 打烊流程：①加锁设 stop=true（标记打烊）→ ②notify_all 摇铃叫醒所有等着的厨师
 * → ③挨个 join 等所有厨师干完手上的活并退出。注意 stop 必须在锁内设置，
 * 否则 worker 可能在 stop 检查与 wait 之间错过通知（race condition）。
 */
ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::shutdown() noexcept
{
    // shutdown 可能由正常退出路径和析构各调用一次；只允许一方执行 join。
    std::lock_guard<std::mutex> shutdownLock(shutdownMtx);
    {
        // 锁内设置 stop，保证 worker 的 wait 能看到 stop 变化
        std::unique_lock<std::mutex> lock(mtx);
        stop=true;
    }

    // 将所有的等待线程唤醒
    cv.notify_all();

    // 将所有的工作线程回收
    for(auto &worker: workers)
    {
        if (worker.joinable())
            worker.join();
    }
}

/**
 * @brief 添加任务到队列
 * @param task 可调用对象
 * @return true=入队成功；false=队列满（背压）
 *
 * 【通俗解释】
 * 加锁→检查队列长度是否到上限→没满就 push→解锁→notify_one 叫一个厨师来领。
 * 满了直接返回 false，让上层（Executor）感知过载并降级（如返回 503）。
 * 用 move 转移 task 避免 function 拷贝开销。
 */
bool ThreadPool::addTask(std::function<void()> task)
{
    {
        std::unique_lock<std::mutex> lock(mtx); // 获取锁资源，保护条件变量
        // ----- 背压检查：队列满则拒单，避免 OOM -----
        // shutdown 开始后不再接新任务，否则 join 的任务集合没有稳定边界。
        if (stop || !task || tasks.size() >= MAX_THREAD_POOL_QUEUE)
        {
            return false;
        }
        // 后续扩展任务主要就是放到去多态化task即可
        tasks.push(std::move(task));
    }   

      // 唤醒一个等待的线程开始工作
    cv.notify_one(); // 唤醒一个线程去工作
    return true;
}
