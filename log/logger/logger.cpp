#include "logger.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>

Logger &Logger::instance()
{
    static Logger logger;
    return logger;
    // TODO: 在此处插入 return 语句
}

void Logger::log(LogLevel level, const std::string &msg, const char *file, int line)
{

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);

    std::tm tm = *std::localtime(&t);

    std::cout << std::put_time(&tm, "%F %T") << " ";

    switch (level)
    {
    case LogLevel::DEBUG:
        std::cout << "[DEBUG]   ";
        break;
    case LogLevel::INFO:
        std::cout << "[INFO ]   ";
        break;
    case LogLevel::WARN:
        std::cout << "[WARN ]   ";
        break;
    case LogLevel::ERROR:
        std::cout << "[ERROR]   ";
        break;
    case LogLevel::HTTP:
        std::cout << "[HTTP ]   ";
        break;
    case LogLevel::REACTOR:
        std::cout << "[REACTOR] ";
        break;
    case LogLevel::ROUTER:
        std::cout << "[ROUTER]  ";
        break;
    case LogLevel::DB:
        std::cout << "[DB    ]  ";
        break;
    }

    // 4. 线程ID + 文件名 + 行号 + 消息
    std::cout << "TID:" << std::this_thread::get_id()
              << " | " << file << ":" << line
              << " | " << msg << "\n";
}
