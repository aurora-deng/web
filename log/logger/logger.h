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
    struct Impl;
    Impl* impl_;
};


#define LOG_INFO(msg)   \
    Logger::instance().log(LogLevel::INFO,msg,__FILE__,__LINE__)

#define LOG_ERROR(msg)   \
    Logger::instance().log(LogLevel::ERROR,msg,__FILE__,__LINE__)

#define LOG_WARN(msg)   \
    Logger::instance().log(LogLevel::WARN,msg,__FILE__,__LINE__)

#define LOG_DEBUG(msg)   \
    Logger::instance().log(LogLevel::DEBUG,msg,__FILE__,__LINE__)


#define LOG_HTTP(msg)   \
    Logger::instance().log(LogLevel::HTTP,msg,__FILE__,__LINE__)

#define LOG_REACTOR(msg)   \
    Logger::instance().log(LogLevel::REACTOR,msg,__FILE__,__LINE__)

#define LOG_ROUTER(msg)   \
    Logger::instance().log(LogLevel::ROUTER,msg,__FILE__,__LINE__)

#define LOG_DB(msg)   \
    Logger::instance().log(LogLevel::DB,msg,__FILE__,__LINE__)


#endif