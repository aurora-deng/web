// ============================================================
// 文件：logger.h
// 职责：异步日志系统的"对外窗口"——声明日志级别、Logger 单例类与日志宏
//
// 【生活比喻：酒店值班日志的"规章制度牌"】
//   logger.cpp 是真正干活的"文员"，而这个头文件则是贴在墙上的"规章制度牌"：
//   它告诉所有人（1）日志分哪些等级（DEBUG/INFO/WARN/ERROR…），
//   （2）怎么调用记事功能（LOG_INFO 等宏），（3）Logger 是全酒店唯一的
//   日记本管理员（单例）。至于文员具体怎么双缓冲、怎么后台打印，那是
//   .cpp 里的内部细节（Pimpl），墙上的牌子不需要透露。
//
// 关键技术点（初学者重点理解）：
//   1. 日志级别分级：从 DEBUG（调试细节）到 ERROR（严重错误），便于按需过滤。
//   2. 宏定义开关：压测时把所有 LOG_XXX 宏定义为空操作（do{}while(0)），
//      零开销，消除日志对性能的影响；需要时改回真正调用即可。
//   3. Pimpl 惯用法：头文件只暴露 struct Impl 的前置声明 + 指针，实现细节
//      藏在 .cpp，减少头文件包含依赖、加快编译。
//   4. 单例模式：全进程一个 Logger 实例，通过 instance() 获取。
// ============================================================
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

/**
 * @brief 日志级别枚举
 *
 * 【日志级别的"紧急程度阶梯"】
 *   DEBUG   —— 调试细节，开发时看，生产关掉
 *   INFO    —— 常规信息，如"服务器已启动"
 *   WARN    —— 警告，程序还能跑但要注意（如连接接近上限）
 *   ERROR   —— 错误，某项操作失败（如 send 返回 -1）
 *   HTTP    —— HTTP 请求专用，便于单独过滤访问日志
 *   REACTOR —— Reactor 内部事件，调试事件循环时用
 *   ROUTER  —— 路由匹配过程，调试路由时用
 *   DB      —— 数据库操作（预留）
 */
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

/**
 * @brief 异步日志器：双缓冲 + 后台写线程
 *
 * 【酒店唯一的值班日记本管理员】
 *   Logger 是全进程单例，内部有一个后台写线程和双缓冲结构。业务线程调用
 *   log() 把格式化好的日志塞进前台缓冲，后台线程定期 swap 并输出。
 *   使用方式：Logger::instance().log(LogLevel::INFO, "消息", __FILE__, __LINE__)
 *   或更方便地用下面的 LOG_INFO / LOG_ERROR 等宏。
 */
class Logger
{
    public:
    /**
     * @brief 获取全局唯一 Logger 实例（Meyers 单例，线程安全）
     * @return Logger 单例引用
     */
    static Logger& instance();

    /**
     * @brief 记录一条日志（业务线程调用，异步输出）
     * @param level 日志级别
     * @param msg   日志正文
     * @param file  源文件名（通常由 __FILE__ 宏传入）
     * @param line  源码行号（通常由 __LINE__ 宏传入）
     */
    void log(LogLevel level,const std::string& msg,const char *file,int line);

    private:
    Logger();   // 单例：构造私有，外部只能通过 instance() 获取
    ~Logger();  // 析构时确保残余日志全部输出

    // 异步日志：后台写线程（"文员"）
    std::thread writer_;
    std::atomic<bool> running_{true}; // 生命标志：false 时通知写线程收尾

    // Pimpl 惯用法：头文件只持有前置声明的指针，实现细节藏在 .cpp
    // 【为什么用 Pimpl】减少头文件依赖、加快编译、隐藏 mutex/buffer 等内部结构
    struct Impl;  // 前置声明，真正定义在 logger.cpp
    Impl* impl_;  // 指向内部实现（双缓冲、互斥锁、条件变量等）
};


// 恢复日志时可把下方空宏替换成对应的 Logger::instance().log(...) 调用。
// LOG_INFO    -> Logger::instance().log(LogLevel::INFO,msg,__FILE__,__LINE__)

// LOG_ERROR   -> Logger::instance().log(LogLevel::ERROR,msg,__FILE__,__LINE__)

// LOG_WARN    -> Logger::instance().log(LogLevel::WARN,msg,__FILE__,__LINE__)

// LOG_DEBUG   -> Logger::instance().log(LogLevel::DEBUG,msg,__FILE__,__LINE__)


// LOG_HTTP    -> Logger::instance().log(LogLevel::HTTP,msg,__FILE__,__LINE__)

// LOG_REACTOR -> Logger::instance().log(LogLevel::REACTOR,msg,__FILE__,__LINE__)

// LOG_ROUTER  -> Logger::instance().log(LogLevel::ROUTER,msg,__FILE__,__LINE__)

// LOG_DB      -> Logger::instance().log(LogLevel::DB,msg,__FILE__,__LINE__)


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
