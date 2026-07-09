#ifndef LOGGER_H
#define LOGGER_H

#include<string>
#include<atomic>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <iomanip>
 
enum class LogLevel
{
    INFO,
    DEBUG,
    WARN,
    ERROR,
    HTTP,
    REACTOR,
    ROUTER,
    DB
};

class Logger
{
    public:
    static Logger& instance();

    void log(LogLevel level,const std::string& msg,const char *file,int line);

    private:
    Logger();
    ~Logger();
    
    // 异步日志：后台线程
    std::thread writer_;
    std::atomic<bool> running_{true};

    // 环形缓冲区（无锁单生产-单消费不够，多生产需要轻量锁
    // 使用双缓冲策略前台缓冲业务线程写入，后台缓冲供线程输出
    // 学习点：通过使用空结构体声明加指针，之后通过部分结构体继承与Impl来实现通过impl_指向对应的内存块，实现最高效利用内存
    struct Impl;
    Impl* impl_;
};


// #define LOG_INFO(msg)   \
//     Logger::instance().log(LogLevel::INFO,msg,__FILE__,__LINE__)

// #define LOG_ERROR(msg)   \
//     Logger::instance().log(LogLevel::ERROR,msg,__FILE__,__LINE__)

// #define LOG_WARN(msg)   \
//     Logger::instance().log(LogLevel::WARN,msg,__FILE__,__LINE__)

// #define LOG_DEBUG(msg)   \
//     Logger::instance().log(LogLevel::DEBUG,msg,__FILE__,__LINE__)


// #define LOG_HTTP(msg)   \
//     Logger::instance().log(LogLevel::HTTP,msg,__FILE__,__LINE__)

// #define LOG_REACTOR(msg)   \
//     Logger::instance().log(LogLevel::REACTOR,msg,__FILE__,__LINE__)

// #define LOG_ROUTER(msg)   \
//     Logger::instance().log(LogLevel::ROUTER,msg,__FILE__,__LINE__)

// #define LOG_DB(msg)   \
//     Logger::instance().log(LogLevel::DB,msg,__FILE__,__LINE__)


// ============================================================
// 压测纯净版：所有日志宏定义为空操作
// 目的：消除日志对压测结果的任何影响，测出服务器真实性能上限
// 编译时宏展开为空，零开销（编译器会完全优化掉）
// 压测完成后如需恢复日志，把下面的宏改回：
//   #define LOG_INFO(msg) Logger::instance().log(LogLevel::INFO,msg,__FILE__,__LINE__)
// ============================================================
#define LOG_INFO(msg)   do {} while(0)
#define LOG_ERROR(msg)  do {} while(0)
#define LOG_WARN(msg)   do {} while(0)
#define LOG_DEBUG(msg)  do {} while(0)
#define LOG_HTTP(msg)   do {} while(0)
#define LOG_REACTOR(msg) do {} while(0)
#define LOG_ROUTER(msg) do {} while(0)
#define LOG_DB(msg)     do {} while(0)
 

#endif