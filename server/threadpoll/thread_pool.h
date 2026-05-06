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

class ThreadPool{
    private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex mtx;
    std::condition_variable cv; // 用于通知线程有新任务的条件变量
    bool stop=false;

    void startThreadPool(size_t numThreads) ;
    public:
    ThreadPool(int threadPoolSize);
    ~ThreadPool();

    void addTask(std::function<void()> task);        //添加任务到线程池里面去
};


#endif