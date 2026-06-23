#include"thread_pool.h"


ThreadPool::ThreadPool(int threadPoolSize):stop(false)
{
    startThreadPool(threadPoolSize);
}

void ThreadPool::startThreadPool(size_t numThreads)
{
    // 循环创建线程
    for(int i=0;i<numThreads;i++)
    {
        // 创建线程并放到对应容器
        workers.emplace_back([this]
        {
            while (true)
            {
                std::function<void()>task;  //创建任务

                {
                    std::unique_lock<std::mutex> lock(this->mtx);               //加锁
                    this->cv.wait(lock,[this]{                                  //等待条件变量通知
                        return this->stop || !this->tasks.empty();
                });

                if(this->stop&&tasks.empty()) return;            //当线程池停止或者任务队列为空，退出线程


                task=std::move(tasks.front());          //将任务取出
                tasks.pop();                            //移除消息队列
            }
            task();         //执行队伍

            }});
    }
}


ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(mtx);
        stop=true;
    }

    // 将所有的等待线程唤醒
    cv.notify_all();

    // 将所有的工作线程回收
    for(auto &worker: workers)
    {
        worker.join();
    }
}

bool ThreadPool::addTask(std::function<void()> task)
{
    {
        std::unique_lock<std::mutex> lock(mtx); // 获取锁资源，保护条件变量
        // 背压修复处：线程池队列满时拒绝新任务，防止内存无限增长
        if (tasks.size() >= MAX_THREAD_POOL_QUEUE)
        {
            return false;
        }
        tasks.push(task);
    }

      // 唤醒一个等待的线程开始工作
    cv.notify_one(); // 唤醒一个线程去工作
    return true;
}